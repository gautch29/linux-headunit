// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/ui_transport.hpp>
#include <headunit/ultra_ui_envelope.hpp>
#include <headunit/binary_plist.hpp>
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace headunit::theme::script {
namespace {
std::string canonical(std::string value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' ||
      value[23] != '-' || value.find_first_not_of("0123456789abcdefABCDEF-") != value.npos)
    return value;
  for (auto& c : value)
    if (c >= 'A' && c <= 'F') c += 'a' - 'A';
  return value;
}
const PlistValue* field(const PlistValue::Dictionary& dict, std::string_view name) {
  const auto i = dict.find(name);
  return i == dict.end() ? nullptr : &i->second;
}
UiFieldShape shape(const PlistValue* value) {
  // PlistValue's bounded subset has exactly these alternatives, in this order.
  return value ? static_cast<UiFieldShape>(value->value.index() + 1) : UiFieldShape::absent;
}
std::optional<UiCommandDiagnostic> check_plist(std::span<const std::uint8_t> bytes, bool diagnostic,
                                               PlistValue::Bytes* normalized = nullptr) {
  auto value = decode_binary_plist(bytes);
  auto* dict = std::get_if<PlistValue::Dictionary>(&value.value);
  if (!dict) throw std::invalid_argument("UI session root must be a dictionary");
  // The outer session data is itself a serialized command. Do not permit an
  // opaque DATA object to evade the graph check when the asset parses it next.
  if (const auto i = dict->find("data"); i != dict->end()) {
    const auto* nested = std::get_if<PlistValue::Bytes>(&i->second.value);
    if (!nested) throw std::invalid_argument("UI command must be a bounded binary dictionary");
    auto command = decode_binary_plist(*nested);
    auto* body = std::get_if<PlistValue::Dictionary>(&command.value);
    if (!body) throw std::invalid_argument("UI command must be a bounded binary dictionary");
    // Inspect already decoded trees only. Shape metadata never adds validation
    // rules to the asset-owned session or changes delivery of unusual commands.
    const auto* outer_type = field(*dict, "type");
    const auto* kind = outer_type ? std::get_if<std::int64_t>(&outer_type->value) : nullptr;
    bool converted_timestamp = false;
    if (normalized && kind && *kind == 2) {
      const auto* type = field(*body, "type");
      const auto* command_type = type ? std::get_if<std::int64_t>(&type->value) : nullptr;
      const auto data = body->find("data");
      auto* values =
          data == body->end() ? nullptr : std::get_if<PlistValue::Dictionary>(&data->second.value);
      // The phone sends transition timestamps as unsigned 32.32 NTP. The
      // signed controller consumes seconds in data.pts, on the same absolute
      // phone timeline as NativeInput/MediaClock. Convert only an explicitly
      // supplied timestamp; never use reception time or synthesize a clock.
      if (command_type && *command_type >= 1 && *command_type <= 3 && values &&
          !values->contains("pts")) {
        const auto* source = field(*values, "current_ntp_time");
        const auto* ntp = source ? std::get_if<std::int64_t>(&source->value) : nullptr;
        if (ntp && *ntp > 0) {
          const auto stamp = static_cast<std::uint64_t>(*ntp);
          const auto pts = double(stamp >> 32) + double(stamp & 0xffffffffULL) / 4294967296.0;
          values->emplace("pts", PlistValue{pts});
          i->second = PlistValue{encode_binary_plist(command)};
          *normalized = encode_binary_plist(value);
          converted_timestamp = true;
        }
      }
    }
    if (diagnostic && kind && *kind == 2) {
      UiCommandDiagnostic result;
      result.converted_transition_timestamp = converted_timestamp;
      const auto* type = field(*body, "type");
      result.type_shape = shape(type);
      if (type)
        if (const auto* n = std::get_if<std::int64_t>(&type->value)) result.command_type = *n;
      const auto* data = field(*body, "data");
      result.data_shape = shape(data);
      if (data)
        if (const auto* values = std::get_if<PlistValue::Dictionary>(&data->value)) {
          for (std::size_t n = 0; n < result.fields.size(); ++n)
            result.fields[n] = shape(field(*values, result.field_names[n]));
          if (result.command_type == 7 || result.command_type == 8) {
            if (const auto* token = field(*values, "tokenID"))
              if (const auto* n = std::get_if<std::int64_t>(&token->value)) result.focus_token = *n;
          }
          if (result.command_type == 9)
            if (const auto* flag = field(*values, "isTransitioning"))
              if (const auto* value = std::get_if<bool>(&flag->value))
                result.transitioning = *value;
        }
      return result;
    }
  }
  return {};
}
}  // namespace
std::string_view ui_field_shape_name(UiFieldShape value) {
  constexpr std::array names{"absent", "boolean", "integer", "real",
                             "string", "bytes",   "array",   "dictionary"};
  const auto index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : "unknown";
}
UiTransport::UiTransport(Channel& channel, std::uint64_t id, std::vector<DisplayRoute> routes,
                         UiCommandDiagnosticSink sink, std::size_t limit)
    : channel_(channel),
      stream_id_(id),
      routes_(std::move(routes)),
      diagnostic_sink_(std::move(sink)),
      diagnostic_limit_(limit) {
  if (!id || routes_.empty() || routes_.size() > 8)
    throw std::invalid_argument("invalid UI stream or display routes");
  if (limit < 1 || limit > 256)
    throw std::invalid_argument("UI command diagnostic limit must be 1..256");
  std::set<std::string> ids, displays;
  const auto fixture = encode_binary_plist(PlistValue{PlistValue::Dictionary{}});
  for (const auto& route : routes_) {
    (void)channel_.runtime(route.asset_display);
    // Reuse the receiver codec's bounded panel-ID validation.
    (void)encode_ultra_ui_envelope({route.phone_uuid, fixture});
    if (!ids.insert(canonical(route.phone_uuid)).second ||
        !displays.insert(route.asset_display).second)
      throw std::invalid_argument("duplicate UI display route");
    phone_ids_.push_back(route.phone_uuid);
  }
}
UiTransport::~UiTransport() {
  close();
}
void UiTransport::start() {
  if (started_ || closed_) throw std::logic_error("UI stream is single-use");
  channel_.set_connected(true);
  started_ = true;
}
void UiTransport::close() {
  if (closed_) return;
  channel_.set_connected(false);
  pending_.reset();
  pending_diagnostic_.reset();
  closed_ = true;
}
TransportDelivery UiTransport::receive(std::uint64_t id, const DataMessage& message) {
  rejection_reason_.clear();
  if (id != stream_id_) return TransportDelivery::wrong_stream;
  if (!started_ || closed_) return TransportDelivery::closed;
  if (message.kind != DataMessageKind::asynchronous || message.type != 0x636f6d6d ||
      message.error) {
    rejection_reason_ = "unsupported DataStream header kind=" + std::to_string(int(message.kind)) +
                        " type=" + std::to_string(message.type) +
                        " error=" + std::to_string(message.error);
    return TransportDelivery::unsupported;
  }
  UltraUiEnvelope envelope;
  // First route against the configured set. Unknown IDs cannot allocate a VM.
  try {
    envelope = decode_ultra_ui_envelope(message.payload, phone_ids_);
  } catch (const std::invalid_argument& error) {
    rejection_reason_ = std::string(error.what()).substr(0, 256);
    return TransportDelivery::malformed;
  }
  const auto route = std::find_if(routes_.begin(), routes_.end(), [&](const auto& r) {
    return r.phone_uuid == envelope.display_uuid;
  });
  if (route == routes_.end()) return TransportDelivery::unknown_display;
  const auto panel = std::size_t(route - routes_.begin());
  std::optional<UiCommandDiagnostic> diagnostic;
  try {
    diagnostic = check_plist(envelope.binary_plist,
                             diagnostic_sink_ && diagnostic_counts_[panel] < diagnostic_limit_,
                             &envelope.binary_plist);
  } catch (const std::invalid_argument& error) {
    rejection_reason_ = std::string(error.what()).substr(0, 256);
    return TransportDelivery::malformed;
  }
  const auto result =
      channel_.receive(ui::Message{route->asset_display, std::move(envelope.binary_plist)});
  switch (result) {
    case Delivery::accepted:
      diagnose(std::move(diagnostic), panel, UiCommandDiagnostic::Direction::incoming_vm_queue,
               channel_.runtime(route->asset_display).message_generation());
      return TransportDelivery::accepted;
    case Delivery::unknown_display:
      return TransportDelivery::unknown_display;
    case Delivery::backpressure:
      return TransportDelivery::backpressure;
    case Delivery::malformed:
      return TransportDelivery::malformed;
    case Delivery::faulted:
      return TransportDelivery::faulted;
  }
  throw std::logic_error("invalid UI delivery result");
}
const DataMessage* UiTransport::pending() {
  if (!started_ || closed_) return nullptr;
  if (pending_ && !current()) {
    pending_.reset();
    pending_diagnostic_.reset();
  }
  if (pending_) return &*pending_;
  const auto outgoing = channel_.take_outgoing_message();
  if (!outgoing) return nullptr;
  try {
    const auto& envelope = *outgoing;
    const auto route = std::find_if(routes_.begin(), routes_.end(), [&](const auto& r) {
      return r.asset_display == envelope.display_uuid;
    });
    if (route == routes_.end()) throw std::invalid_argument("unrouted outgoing UI display");
    pending_panel_ = std::size_t(route - routes_.begin());
    pending_diagnostic_ = check_plist(
        envelope.data, diagnostic_sink_ && diagnostic_counts_[pending_panel_] < diagnostic_limit_);
    pending_ = DataMessage{};
    pending_->type = 0x636d6e64;  // cmnd outbound; the phone uses comm inbound.
    pending_->payload = encode_ultra_ui_envelope({route->phone_uuid, envelope.data});
    pending_display_ = route->asset_display;
    pending_generation_ = channel_.runtime(pending_display_).message_generation();
  } catch (...) {
    close();
    throw;
  }
  return &*pending_;
}
bool UiTransport::current() const {
  return pending_ &&
         channel_.runtime(pending_display_).message_generation() == pending_generation_ &&
         !channel_.runtime(pending_display_).stats().faulted;
}
void UiTransport::accepted() {
  if (!current()) {
    pending_.reset();
    pending_diagnostic_.reset();
    throw std::logic_error("no current pending UI message");
  }
  diagnose(std::move(pending_diagnostic_), pending_panel_,
           UiCommandDiagnostic::Direction::outgoing_worker_queue, pending_generation_);
  pending_diagnostic_.reset();
  pending_.reset();
}
void UiTransport::diagnose(std::optional<UiCommandDiagnostic> event, std::size_t panel,
                           UiCommandDiagnostic::Direction direction,
                           std::uint64_t generation) noexcept {
  if (!event || !diagnostic_sink_ || diagnostic_counts_[panel] >= diagnostic_limit_) return;
  event->panel = static_cast<std::uint8_t>(panel);
  event->direction = direction;
  event->generation = generation;
  event->count = ++diagnostic_counts_[panel];
  event->budget_exhausted = event->count == diagnostic_limit_;
  event->monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  try {
    diagnostic_sink_(*event);
  } catch (...) {
  }  // Observation cannot change delivery.
}
}  // namespace headunit::theme::script
