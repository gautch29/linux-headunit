// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "headunit/airplay_command.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace headunit;
using Value = PlistValue;
using Dictionary = Value::Dictionary;
using Array = Value::Array;
Value number(std::int64_t value) {
  return Value{value};
}
void check(bool valid, const char* reason) {
  if (!valid) throw std::runtime_error(reason);
}
auto command(std::string type, Dictionary params = {}) {
  return encode_binary_plist(
      Value{Dictionary{{"type", Value{std::move(type)}}, {"params", Value{std::move(params)}}}});
}
template <class F>
void rejects(F&& call) {
  try {
    call();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid command accepted");
}
}  // namespace

int main() try {
  using namespace headunit;
  const Value screen{
      Dictionary{{"resourceID", number(1)}, {"entity", number(1)}, {"permanentEntity", number(2)}}};
  const Value speech{
      Dictionary{{"appStateID", number(1)}, {"entity", number(1)}, {"speechMode", number(3)}}};
  const auto complete = decode_airplay_command(command(
      "modesChanged", {{"resources", Value{Array{screen}}}, {"appStates", Value{Array{speech}}}}));
  check(complete.kind == AirplayCommandKind::modes_changed && complete.modes,
        "mode observation not decoded");
  AirplayModesState state;
  check(!state.resources[0] && !state.apps[0] && !state.observed_at && state.revision == 0,
        "unknown mode became an owner");
  const auto now = std::chrono::steady_clock::now();
  state.apply(*complete.modes, now);
  check(state.resources[0] == AirplayResourceMode{1, 2} && state.apps[0] == AirplayAppMode{1, 3} &&
            !state.resources[1] && !state.apps[1],
        "mode observation lost fields or manufactured missing state");
  const auto delta = decode_airplay_command(command(
      "modesChanged",
      {{"resources",
        Value{Array{Value{Dictionary{{"resourceID", number(2)}, {"entity", number(2)}}}}}}}));
  state.apply(*delta.modes, now);
  check(state.resources[0] == AirplayResourceMode{1, 2} &&
            state.resources[1] == AirplayResourceMode{2, {}} &&
            state.apps[0] == AirplayAppMode{1, 3} && state.revision == 2,
        "partial observation erased unrelated state");
  check(airplay_modes_summary(AirplayModesState{}) ==
            "mainScreen=-/- mainAudio=-/- speech=- phoneCall=- turnByTurn=-",
        "unobserved modes summary invented an owner");
  check(airplay_modes_summary(state) ==
            "mainScreen=1/2 mainAudio=2/- speech=1 phoneCall=- turnByTurn=-",
        "modes summary lost current or permanent ownership");
  rejects([&] { state.apply(*delta.modes, now - std::chrono::nanoseconds(1)); });
  check(state.revision == 2 && state.observed_at == now, "bad clock partially mutated state");
  const auto later = now + std::chrono::seconds(1);
  state.apply({}, later);
  check(state.observed_at == later && state.resource_observed_at[0] == now &&
            state.resource_observed_at[1] == now && state.app_observed_at[0] == now &&
            !state.app_observed_at[1],
        "empty delta renewed unrelated resource/app dates");
  check(airplay_resource_owner({}) == AirplayResourceOwner::unobserved &&
            airplay_resource_owner(AirplayResourceMode{1, 2}) == AirplayResourceOwner::controller &&
            airplay_resource_owner(AirplayResourceMode{2, 1}) == AirplayResourceOwner::accessory &&
            airplay_resource_owner(AirplayResourceMode{0, 1}) == AirplayResourceOwner::unknown &&
            airplay_resource_owner(AirplayResourceMode{99, 1}) == AirplayResourceOwner::unknown,
        "permanent or unrecognized owner created a current permission");
  AirplayModesState successor;
  check(!successor.resources[0] && !successor.observed_at, "mode ownership leaked to successor");
  for (const auto speech_value : {-1LL, 0LL, 4294967295LL}) {
    const auto observed = decode_airplay_command(command(
        "modesChanged",
        {{"resources",
          Value{Array{Value{Dictionary{{"resourceID", number(2)}, {"entity", number(1)}}}}}},
         {"appStates", Value{Array{Value{Dictionary{{"appStateID", number(1)},
                                                    {"entity", number(0)},
                                                    {"speechMode", number(speech_value)}}}}}}}));
    successor.apply(*observed.modes, later);
    check(successor.apps[0] == AirplayAppMode{0, speech_value} &&
              airplay_resource_owner(successor.resources[1]) == AirplayResourceOwner::controller,
          "speech sentinel rejected an otherwise valid resource observation");
  }
  for (const auto& invalid : {Value{std::int64_t{-2}}, Value{std::int64_t{4294967296}}, Value{true},
                              Value{1.5}, Value{std::string{"-1"}}}) {
    rejects([&] {
      decode_airplay_command(command(
          "modesChanged",
          {{"appStates",
            Value{Array{Value{Dictionary{
                {"appStateID", number(1)}, {"entity", number(0)}, {"speechMode", invalid}}}}}}}));
    });
  }
  check(decode_airplay_command(command("disableBluetooth")).kind ==
                AirplayCommandKind::disable_bluetooth &&
            decode_airplay_command(command("suggestUI")).kind == AirplayCommandKind::suggest_ui &&
            decode_airplay_command(command("futurePrivateCommand")).kind ==
                AirplayCommandKind::unsupported,
        "unsupported commands incorrectly classified");
  const auto unknown = decode_airplay_command(command("futurePrivateCommand"));
  check(unknown.unsupported_fingerprint &&
            unknown.unsupported_fingerprint ==
                decode_airplay_command(command("futurePrivateCommand", {{"secret", Value{true}}}))
                    .unsupported_fingerprint &&
            unknown.unsupported_fingerprint !=
                decode_airplay_command(command("differentCommand")).unsupported_fingerprint &&
            !complete.unsupported_fingerprint,
        "unknown command coverage retained parameters or confused known categories");
  const Value bad_id{Dictionary{{"resourceID", number(3)}, {"entity", number(1)}}};
  const Value bad_type{Dictionary{{"resourceID", number(1)}, {"entity", Value{true}}}};
  const Value bad_entity{Dictionary{{"resourceID", number(1)}, {"entity", number(-1)}}};
  const Value bad_speech{
      Dictionary{{"appStateID", number(1)}, {"entity", number(1)}, {"speechMode", Value{1.5}}}};
  for (const auto& params : std::vector<Dictionary>{{},
                                                    {{"resources", Value{true}}},
                                                    {{"resources", Value{Array{screen, screen}}}},
                                                    {{"resources", Value{Array{screen, bad_id}}}},
                                                    {{"resources", Value{Array{bad_type}}}},
                                                    {{"resources", Value{Array{bad_entity}}}},
                                                    {{"appStates", Value{Array{bad_speech}}}},
                                                    {{"unknown", number(1)}}})
    rejects([&] { decode_airplay_command(command("modesChanged", params)); });
  rejects([] { decode_airplay_command({}); });
  rejects([] { decode_airplay_command(command(std::string(65, 'x'))); });
  std::cout << "bounded commands, partial mode observations, atomic rejection and successor "
               "isolation passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
