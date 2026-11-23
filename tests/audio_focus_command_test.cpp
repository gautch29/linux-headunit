// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/audio_focus_command.hpp>
#include <headunit/binary_plist.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace headunit;
void check(bool value, const char* why) {
  if (!value) throw std::runtime_error(why);
}
auto command(std::string type, PlistValue::Dictionary params = {}) {
  return encode_binary_plist(PlistValue{PlistValue::Dictionary{
      {"type", PlistValue{std::move(type)}}, {"params", PlistValue{std::move(params)}}}});
}
template <class F>
void rejects(F&& f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "bad audio focus command accepted");
}
}  // namespace
int main() try {
  using namespace headunit;
  const auto duck = decode_audio_focus_command(command("duckAudio"));
  check(duck && duck->gain == .2 && duck->duration_ms == 500, "known duck defaults lost");
  const auto release = decode_audio_focus_command(command("unduckAudio"));
  check(release && release->gain == 1 && release->duration_ms == 500,
        "known release defaults lost");
  check(
      decode_audio_focus_command(command("unduckAudio", {{"volume", PlistValue{true}}}))->gain == 1,
      "release interpreted a volume field that the established handler ignores");
  const auto fractional = decode_audio_focus_command(
      command("duckAudio", {{"volume", PlistValue{-12.5}}, {"durationMs", PlistValue{17.25}}}));
  check(fractional && std::abs(fractional->gain - std::pow(10., -12.5 / 20.)) < 1e-12 &&
            fractional->duration_ms == 17.25,
        "dB/fractional millisecond contract lost");
  check(decode_audio_focus_command(
            command("duckAudio", {{"volume", PlistValue{std::int64_t(-144)}},
                                  {"durationMs", PlistValue{std::int64_t(0)}}}))
                ->gain == 0,
        "silent/immediate focus rejected");
  check(decode_audio_focus_command(command("duckAudio", {{"volume", PlistValue{std::int64_t(5)}}}))
                ->gain == 1,
        "phone duck amplified music above unity");
  PlistValue::Array resources(40);
  for (std::size_t i = 0; i < resources.size(); ++i)
    resources[i].value.emplace<std::int64_t>(i);
  check(
      !decode_audio_focus_command(command("modesChanged", {{"resources", PlistValue{resources}}})),
      "unimplemented command lost its diagnostic route");
  for (const auto& invalid :
       {command("duckAudio", {{"volume", PlistValue{true}}}),
        command("duckAudio", {{"durationMs", PlistValue{-1.}}}),
        command("duckAudio", {{"durationMs", PlistValue{60000.1}}}),
        command("duckAudio", {{"volume", PlistValue{std::string("-12")}}}),
        command("duckAudio", {{"unknown", PlistValue{std::int64_t(0)}}}),
        command("duckAudio", {{"padding", PlistValue{std::string(9000, 'x')}}})})
    rejects([&] { decode_audio_focus_command(invalid); });
  rejects([] { decode_audio_focus_command({}); });
  auto truncated = command("duckAudio");
  truncated.pop_back();
  rejects([&] { decode_audio_focus_command(truncated); });
  std::cout << "bounded phone duck/release commands, defaults, dB and numeric units passed\n";
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
