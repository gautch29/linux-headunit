// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "drive_mode_probe.hpp"

#include <headunit/binary_plist.hpp>
#include <headunit/theme/gauge_input.hpp>
#include <headunit/theme/script_channel.hpp>
#include <headunit/theme/protocol.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace headunit::theme::bench {
namespace {
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
std::uint64_t milliseconds(headunit::Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}
struct ModeRequest {
  std::string identifier;
};
const headunit::PlistValue::Dictionary& dictionary(const headunit::PlistValue& value,
                                                   std::string_view field) {
  const auto* result = std::get_if<headunit::PlistValue::Dictionary>(&value.value);
  if (!result)
    throw std::runtime_error("mode probe expected dictionary at " + std::string(field) +
                             "; variant=" + std::to_string(value.value.index()));
  return *result;
}
std::int64_t integer(const headunit::PlistValue& value) {
  if (const auto* result = std::get_if<std::int64_t>(&value.value)) return *result;
  throw std::runtime_error("mode probe expected an integer command field");
}
}  // namespace

void drive_mode_probe(const script::ResourceReader& asset, std::span<const std::uint8_t> bytes) {
  // LocalProfile also accepts the generator's bounded XML form. Apply graph
  // preflight to binary input; both forms pass its strict profile validation.
  if (bytes.size() >= 8 &&
      std::string_view(reinterpret_cast<const char*>(bytes.data()), 8) == "bplist00")
    headunit::preflight_binary_plist(bytes, {65536, 4096, 262144, 32});
  for (const std::string display : {"Center_Display", "Cluster_Display"}) {
    auto profile = vehicle::decode_local_profile(bytes);
    const vehicle::VehicleInputBinding* mapping = nullptr;
    std::optional<std::int64_t> identifiers_iid;
    for (const auto& input : profile.vehicle_inputs)
      if (input.field == "drive_mode") {
        check(!mapping, "mode probe requires one observed drive-mode mapping");
        mapping = &input;
      }
    for (const auto& binding : profile.bindings)
      if (binding.path == std::array<std::string, 3>{"driveState", "driveMode", "identifiers"}) {
        check(!identifiers_iid, "ambiguous authored mode catalogue");
        identifiers_iid = binding.iid;
      }
    check(mapping && mapping->kind == vehicle::ValueKind::integer && identifiers_iid &&
              mapping->semantic_states.size() >= 2,
          "mode probe needs at least two explicitly mapped authored modes");
    std::string catalog_key;
    for (const auto& [key, value] : mapping->semantic_states) {
      (void)value;
      if (!catalog_key.empty()) catalog_key += ',';
      catalog_key += key;
    }
    const auto catalog = headunit::parse_drive_mode_catalog_key(catalog_key);
    check(catalog.has_value(), "invalid semantic probe catalogue");
    headunit::VehicleAgePolicy ages;
    ages.drive_mode = std::chrono::milliseconds(250);
    auto source = std::make_unique<headunit::VehicleObservationCache>(ages, catalog);
    auto* observations = source.get();
    const auto origin = headunit::Clock::now();
    observations->begin_generation(1, origin, origin);
    vehicle::GaugeInput gauges(profile, std::move(source));
    const auto ids = std::array{mapping->iid, *identifiers_iid};
    const auto initial = profile.source->snapshot(ids, milliseconds(origin));
    check(initial.at(*identifiers_iid).value.has_value(), "missing immutable authored catalogue");
    const auto authored = std::get<opack::Array>(initial.at(*identifiers_iid).value->data);
    std::vector<std::string> authored_identifiers;
    for (const auto& identifier : authored)
      authored_identifiers.push_back(std::get<std::string>(identifier.data));

    script::Limits limits;
    limits.startup_ms = 10000;
    limits.frame_ms = 1000;
    script::Channel channel(asset, {{display, 1920, 720}}, limits);
    auto& runtime = channel.runtime(display);
    runtime.bind_vehicle(profile.source, profile.bindings);
    runtime.bind_verification_reporter([&](const script::VerificationReport& report) {
      check(report.display_identifier == display, "mode probe verified-UI display mismatch");
      // Declarations only: this CPU/session probe checks no pixels or output.
    });
    channel.set_connected(true);
    gauges.pump(headunit::Clock::now());
    runtime.evaluate("require 'main'", "original-observed-mode-probe");
    runtime.set_foregrounded(true);  // Simulated host state, never a display receipt.
    std::vector<ModeRequest> requests;
    unsigned unknown_requests = 0;
    std::uint64_t peer_sequence = 0, peer_session = 0;
    unsigned resets = 0;
    const auto tick = [&] {
      const auto now = headunit::Clock::now();
      gauges.pump(now);
      // Synthetic frame context for this offline VM; not phone/video timing.
      runtime.tick(std::chrono::duration<double>(now - origin).count());
      unsigned processed = 0;
      while (auto message = channel.take_outgoing_message()) {
        check(++processed <= 32, "mode probe UI output budget exceeded");
        const auto packet = decode(message->data);
        if (packet.type == MessageType::reset) {
          check(++resets <= 2, "mode probe session failed to remain ready");
          peer_sequence = 0;
          peer_session = packet.session;
        }
        if (packet.type == MessageType::command) {
          const auto value = headunit::decode_binary_plist(packet.data);
          const auto& command = dictionary(value, "command");
          if (integer(command.at("type")) == 0x14) {
            const auto& mode = dictionary(command.at("data"), "data").at("driveMode");
            const auto* identifier = std::get_if<std::string>(&mode.value);
            check(identifier && identifier->size() <= 128,
                  "mode probe expected a bounded driveMode string");
            check(requests.size() < 64, "mode probe request retention exceeded");
            if (std::find(authored_identifiers.begin(), authored_identifiers.end(), *identifier) !=
                authored_identifiers.end())
              requests.push_back({*identifier});
            else {
              check(++unknown_requests <= 64, "mode probe unknown request budget exceeded");
            }
          }
        }
        if (packet.type == MessageType::reset || packet.type == MessageType::command) {
          Packet reply = packet;
          reply.type =
              packet.type == MessageType::reset ? MessageType::reset_complete : MessageType::ack;
          reply.sequence = ++peer_sequence;
          reply.acknowledgement = packet.sequence;
          reply.data.clear();
          reply.no_ack = false;
          check(channel.receive(ui::Message{display, encode(reply)}) == script::Delivery::accepted,
                "mode probe synthetic UI reply was rejected");
        }
      }
      check(!runtime.stats().faulted, "mode probe signed VM faulted");
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    };
    const auto settle = [&] {
      for (unsigned i = 0; i < 8; ++i)
        tick();
    };
    settle();
    check(peer_session && requests.empty(), "unknown initial mode became a valid phone request");
    std::uint64_t sequence = 0;
    const auto apply = [&](const std::string& key, std::uint64_t generation) {
      const auto before = requests.size();
      const auto at = headunit::Clock::now();
      const auto mode = headunit::parse_drive_mode_identifier(key);
      check(mode.has_value(), "invalid selected probe mode");
      const std::array batch{headunit::VehicleObservation{
          "drive_mode", *mode, at, headunit::VehicleObservationOrigin::simulation}};
      observations->accept(generation, ++sequence, batch, at);
      settle();
      check(requests.size() == before + 1, "observed mode did not emit exactly one request");
      const auto expected = std::get<std::int64_t>(mapping->semantic_states.at(key).data);
      check(expected >= 0 && static_cast<std::size_t>(expected) < authored.size() &&
                requests.back().identifier == std::get<std::string>(authored[expected].data),
            "phone request used semantic catalogue order instead of authored mapping");
      return at;
    };
    auto selected = mapping->semantic_states.begin();
    apply(selected->first, 1);
    const auto second = std::next(selected);
    const auto acquired = apply(second->first, 1);
    const auto count = requests.size();
    while (headunit::Clock::now() < acquired + ages.drive_mode + std::chrono::milliseconds(32))
      tick();
    settle();
    const auto expired = profile.source->snapshot(ids, milliseconds(headunit::Clock::now()));
    const auto& expired_value = expired.at(mapping->iid).value;
    check((!expired_value || std::holds_alternative<std::monostate>(expired_value->data)) &&
              requests.size() == count,
          "expired mode retained local state or generated a valid mode request");
    const auto restarted = headunit::Clock::now();
    observations->begin_generation(2, restarted, restarted);
    sequence = 0;
    settle();
    check(requests.size() == count, "source restart replayed an old mode request");
    apply(selected->first, 2);
    // Retire the offline owner; its VM is destroyed at scope exit. This peer
    // fixture never supplies the phone's scene-start/remote-layer commands.
    runtime.set_foregrounded(false);
    channel.set_connected(false);
    std::cout
        << "{\"driveModeProbe\":\"" << display << "\",\"mappedRequests\":" << requests.size()
        << ",\"unknownIdentifierRequests\":" << unknown_requests
        << ",\"unknownSuppressed\":true,\"expirySuppressed\":true,\"restartSuppressed\":true,"
           "\"realAcquisitionDates\":true,\"syntheticVehicleAndPeer\":true,\"livePhone\":false,"
           "\"presentationVerified\":false}\n";
  }
}
}  // namespace headunit::theme::bench
