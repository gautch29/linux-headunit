// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle_command_configuration.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef HEADUNIT_SOURCE_DIR
#define HEADUNIT_SOURCE_DIR "."
#endif

namespace {
using namespace headunit;
namespace control = headunit::vehicle_control;
using namespace std::chrono_literals;

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition))                                                                       \
      throw std::runtime_error("vehicle controls configuration check failed: " #condition); \
  } while (false)

void rejects(const auto& operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error("invalid vehicle controls configuration was accepted");
}

std::string document(std::string_view targets) {
  return "{\"schemaVersion\":1,\"targets\":[" + std::string(targets) + "]}";
}

std::string boolean(std::string_view key = "bench.light.on") {
  return "{\"key\":\"" + std::string(key) + "\",\"kind\":\"boolean\",\"max_age_ms\":500}";
}

const std::string real =
    R"({"key":"bench.temperature_c","kind":"real","writable":true,"max_age_ms":1250,"minimum":16.0,"maximum":30.0,"step":0.5})";
const std::string integral =
    R"({"key":"bench.fan.level","kind":"integer","max_age_ms":500,"minimum":-5,"maximum":5,"step":2})";

std::string replace(std::string text, std::string_view from, std::string_view to) {
  const auto offset = text.find(from);
  CHECK(offset != text.npos);
  text.replace(offset, from.size(), to);
  return text;
}

std::string without_field(std::string text, std::string_view key) {
  const auto first = text.find("\"" + std::string(key) + "\":");
  CHECK(first != text.npos);
  const auto end = text.find(',', first);
  if (end == text.npos) {
    CHECK(first > 1 && text[first - 1] == ',');
    text.erase(first - 1, text.size() - first);
  } else
    text.erase(first, end - first + 1);
  return text;
}

void policy_types_defaults_and_units() {
  const auto policies =
      control::parse_configuration(document(boolean() + "," + integral + "," + real));
  CHECK(policies.size() == 3);
  CHECK(control::key_text(policies[0].key) == "bench.light.on");
  CHECK(policies[0].kind == control::ValueKind::boolean && !policies[0].writable);
  CHECK(!policies[0].minimum && !policies[0].maximum && !policies[0].step);
  CHECK(policies[0].maximum_age == 500ms);
  CHECK(policies[1].kind == control::ValueKind::integer && !policies[1].writable);
  CHECK(policies[1].minimum == -5 && policies[1].maximum == 5 && policies[1].step == 2);
  CHECK(policies[2].kind == control::ValueKind::real && policies[2].writable);
  CHECK(policies[2].minimum == 16.0 && policies[2].maximum == 30.0 && policies[2].step == 0.5);
  CHECK(policies[2].maximum_age == 1250ms);
  for (const auto& policy : policies)
    CHECK(control::valid_policy(policy));
  // Integer JSON numbers also represent real bounds; units are part of the
  // semantic provider contract, never inferred or converted by this loader.
  const auto mixed = control::parse_configuration(document(replace(real, "16.0", "16")));
  CHECK(mixed[0].minimum == 16.0);
  rejects([&] {
    control::parse_configuration(
        document(replace(real, "\"step\":0.5", "\"step\":0.5,\"units\":\"fahrenheit\"")));
  });
  rejects([&] {
    control::parse_configuration(document(replace(real, "max_age_ms", "max_age_seconds")));
  });
}

void exact_schema_and_types() {
  const auto valid = document(boolean());
  for (const auto& invalid :
       {std::string{}, std::string("[]"), std::string("null"), std::string("{}"),
        std::string("{\"schemaVersion\":1}"), std::string("{\"targets\":[]}"),
        std::string("{\"schemaVersion\":1,\"targets\":null}"),
        std::string("{\"schemaVersion\":1,\"targets\":{}}")})
    rejects([&] { control::parse_configuration(invalid); });
  for (const auto version : {"0", "3", "1.0", "1e0", "true", "\"1\"", "null"})
    rejects([&] {
      control::parse_configuration(
          replace(valid, "\"schemaVersion\":1", "\"schemaVersion\":" + std::string(version)));
    });
  rejects([&] {
    control::parse_configuration(replace(valid, "\"targets\":", "\"enabled\":true,\"targets\":"));
  });
  for (const auto kind : {"bool", "int", "Boolean", "number", "string", ""})
    rejects([&] { control::parse_configuration(document(replace(boolean(), "boolean", kind))); });
  for (const auto kind : {"true", "0", "null", "[]"})
    rejects(
        [&] { control::parse_configuration(document(replace(boolean(), "\"boolean\"", kind))); });
  for (const auto value : {"1", "0", "\"true\"", "null", "[]"})
    rejects([&] {
      control::parse_configuration(
          document(replace(real, "\"writable\":true", "\"writable\":" + std::string(value))));
    });
  for (const auto value : {"true", "null", "500.0", "5e2", "\"500\""})
    rejects([&] {
      control::parse_configuration(document(
          replace(boolean(), "\"max_age_ms\":500", "\"max_age_ms\":" + std::string(value))));
    });
  for (const auto value : {"false", "null", "1.0", "1e0", "\"2\""})
    rejects([&] {
      control::parse_configuration(
          document(replace(integral, "\"step\":2", "\"step\":" + std::string(value))));
    });
  for (const auto value : {"false", "null", "\"0.5\"", "1e400", "NaN", "Infinity"})
    rejects([&] {
      control::parse_configuration(
          document(replace(real, "\"step\":0.5", "\"step\":" + std::string(value))));
    });
  for (const auto key : {"key", "kind", "max_age_ms"}) {
    rejects([&] { control::parse_configuration(document(without_field(boolean(), key))); });
  }
  for (const auto field : {"minimum", "maximum", "step"}) {
    // Each numeric target must explicitly declare all three numeric limits.
    rejects([&] { control::parse_configuration(document(without_field(real, field))); });
    rejects([&] { control::parse_configuration(document(without_field(integral, field))); });
    rejects([&] {
      control::parse_configuration(
          document(replace(boolean(), "}", ",\"" + std::string(field) + "\":0}")));
    });
  }
}

void duplicate_and_unknown_keys() {
  rejects([&] { control::parse_configuration(document(boolean() + "," + boolean())); });
  rejects([&] {
    control::parse_configuration(
        document(boolean("bench.light.on") + "," + boolean("bench.light.\\u006fn")));
  });
  rejects([&] {
    control::parse_configuration(replace(document(boolean()), "\"schemaVersion\":1",
                                         "\"schemaVersion\":1,\"schemaVersion\":1"));
  });
  rejects([&] {
    control::parse_configuration(replace(document(boolean()), "\"schemaVersion\":1",
                                         "\"schemaVersion\":1,\"schema\\u0056ersion\":1"));
  });
  rejects([&] {
    control::parse_configuration(
        document(replace(boolean(), "\"kind\":", "\"key\":\"different\",\"kind\":")));
  });
  rejects([&] {
    control::parse_configuration(
        document(replace(real, "\"step\":", "\"st\\u0065p\":0.5,\"step\":")));
  });
  rejects([&] {
    control::parse_configuration(document(replace(boolean(), "}", ",\"label\":\"Lamp\"}")));
  });
  // Key comparison uses decoded semantic text, while distinct case is retained.
  CHECK(
      control::parse_configuration(document(boolean("bench.Light") + "," + boolean("bench.light")))
          .size() == 2);
}

void semantic_key_limits() {
  const std::string maximum_key = "A" + std::string(127, 'x');
  CHECK(control::key_text(control::parse_configuration(document(boolean(maximum_key)))[0].key) ==
        maximum_key);
  CHECK(control::parse_configuration(document(boolean("A0._segment-last"))).size() == 1);
  for (const auto& key :
       {std::string{}, std::string("0first"), std::string(".first"), std::string("first."),
        std::string("first..last"), std::string("first/slash"), std::string("first space"),
        std::string("first:port"), std::string("first\\nline"), std::string("first\\u0000"),
        std::string("first\\u00e9"), maximum_key + "x"})
    rejects([&] { control::parse_configuration(document(boolean(key))); });
}

void target_count_and_age_limits() {
  rejects([] { control::parse_configuration(document("")); });
  std::string targets;
  for (unsigned n = 0; n < control::maximum_targets; ++n) {
    if (!targets.empty()) targets += ',';
    targets += boolean("bench.target" + std::to_string(n));
  }
  CHECK(control::parse_configuration(document(targets)).size() == control::maximum_targets);
  rejects([&] { control::parse_configuration(document(targets + "," + boolean("bench.extra"))); });
  for (const auto age : {"1", "86400000"}) {
    const auto loaded = control::parse_configuration(document(replace(boolean(), "500", age)));
    CHECK(loaded[0].maximum_age == std::chrono::milliseconds(std::stoll(age)));
  }
  for (const auto age : {"0", "-1", "86400001", "9223372036854775807", "9223372036854775808"})
    rejects([&] { control::parse_configuration(document(replace(boolean(), "500", age))); });
}

void numeric_bounds_and_exact_integer_limits() {
  for (const auto& [from, to] :
       {std::pair{"\"minimum\":16.0", "\"minimum\":31.0"},
        std::pair{"\"maximum\":30.0", "\"maximum\":15.0"}, std::pair{"\"step\":0.5", "\"step\":0"},
        std::pair{"\"step\":0.5", "\"step\":-0.5"},
        std::pair{"\"minimum\":16.0", "\"minimum\":-9007199254740992.0"},
        std::pair{"\"maximum\":30.0", "\"maximum\":9007199254740992.0"},
        std::pair{"\"step\":0.5", "\"step\":9007199254740992.0"},
        std::pair{"\"step\":0.5", "\"step\":1e-300"}})
    rejects([&] { control::parse_configuration(document(replace(real, from, to))); });
  for (const auto& [from, to] :
       {std::pair{"\"minimum\":-5", "\"minimum\":6"}, std::pair{"\"maximum\":5", "\"maximum\":-6"},
        std::pair{"\"step\":2", "\"step\":0"}, std::pair{"\"step\":2", "\"step\":-1"},
        std::pair{"\"minimum\":-5", "\"minimum\":-9007199254740992"},
        std::pair{"\"maximum\":5", "\"maximum\":9007199254740992"},
        std::pair{"\"step\":2", "\"step\":9007199254740992"}})
    rejects([&] { control::parse_configuration(document(replace(integral, from, to))); });
  const auto widest = control::parse_configuration(document(
      R"({"key":"bench.exact","kind":"integer","max_age_ms":1,"minimum":-9007199254740991,"maximum":9007199254740991,"step":1})"));
  CHECK(widest[0].minimum == -9007199254740991.0 && widest[0].maximum == 9007199254740991.0);
  CHECK(control::valid_value(widest[0], std::int64_t(-9007199254740991), false));
  CHECK(control::valid_value(widest[0], std::int64_t(9007199254740991), false));
  CHECK(!control::valid_value(widest[0], std::int64_t(9007199254740992), false));
  const auto large_step = control::parse_configuration(document(
      R"({"key":"bench.exactStep","kind":"integer","max_age_ms":1,"minimum":0,"maximum":9007199254740991,"step":9007199254740991})"));
  CHECK(large_step[0].step == 9007199254740991.0);
  const auto wide_real = control::parse_configuration(document(
      R"({"key":"bench.realBounds","kind":"real","max_age_ms":1,"minimum":0,"maximum":9007199254740991,"step":1})"));
  CHECK(wide_real[0].maximum == 9007199254740991.0);
  CHECK(control::valid_value(wide_real[0], 9007199254740991.0, false));
}

void request_grid_and_confirmed_precision() {
  const auto policies =
      control::parse_configuration(document(integral + "," + real + "," + boolean()));
  CHECK(control::valid_value(policies[0], std::int64_t(-5), true));
  CHECK(control::valid_value(policies[0], std::int64_t(-3), true));
  CHECK(!control::valid_value(policies[0], std::int64_t(-4), true));
  CHECK(control::valid_value(policies[0], std::int64_t(-4), false));
  CHECK(!control::valid_value(policies[0], -3.0, false));
  CHECK(!control::valid_value(policies[0], true, false));
  CHECK(control::valid_value(policies[1], 21.5, true));
  CHECK(!control::valid_value(policies[1], 21.25, true));
  CHECK(control::valid_value(policies[1], 21.25, false));
  CHECK(!control::valid_value(policies[1], 31.0, false));
  CHECK(!control::valid_value(policies[1], std::int64_t(21), false));
  CHECK(!control::valid_value(policies[1], false, false));
  CHECK(control::valid_value(policies[2], false, true));
  CHECK(control::valid_value(policies[2], true, false));
  CHECK(!control::valid_value(policies[2], std::int64_t(1), false));
}

void malformed_json_and_size_limit() {
  const auto valid = document(boolean());
  for (const auto& invalid :
       {valid + "false", valid + "\n{}",
        replace(valid, "\"schemaVersion\":1", "\"schemaVersion\":01"),
        replace(valid, "\"schemaVersion\":1", "\"schemaVersion\":+1"),
        replace(valid, "\"targets\":", "\"targets\":/*comment*/"), document(boolean() + ','),
        valid.substr(0, valid.size() - 1), document(boolean("bad\\ud800")),
        document(boolean("bad\\udfff")), document(boolean(std::string("bad") + char(0xff)))})
    rejects([&] { control::parse_configuration(invalid); });
  auto nul = valid;
  nul.push_back('\0');
  rejects([&] { control::parse_configuration(nul); });
  auto bounded = valid;
  bounded.append(65536 - valid.size(), ' ');
  CHECK(control::parse_configuration(bounded).size() == 1);
  bounded += ' ';
  rejects([&] { control::parse_configuration(bounded); });
}

void configuration_cannot_declare_actual_state_or_enable_backend() {
  for (const auto name :
       {"initial", "initialValue", "value", "applied", "confirmed_at", "provider_generation",
        "origin", "backend", "enabled", "interlock", "permission"})
    rejects([&] {
      control::parse_configuration(
          document(replace(boolean(), "}", ",\"" + std::string(name) + "\":true}")));
    });
  for (const auto name : {"backend", "enabled", "initialValues", "confirmedStates"})
    rejects([&] {
      control::parse_configuration(replace(
          document(boolean()), "\"targets\":", "\"" + std::string(name) + "\":true,\"targets\":"));
    });
  const auto defaults = control::parse_configuration(document(boolean()));
  CHECK(!defaults[0].writable);
  // Even a writable declaration contains only a policy, never a confirmed value.
  const auto writable =
      control::parse_configuration(document(replace(boolean(), "}", ",\"writable\":true}")));
  CHECK(writable[0].writable && !writable[0].minimum);
  const auto now = Clock::time_point{100s};
  control::Coordinator read_only(defaults);
  read_only.begin_generation(1, now, now);
  CHECK(!read_only.read(defaults[0].key, now));
  const control::Request denied{1, 1, defaults[0].key, true, now, now + 1s};
  CHECK(read_only.submit(denied, now).reason == control::Reason::not_writable);
  CHECK(!read_only.read(defaults[0].key, now));
  control::Coordinator enabled_policy(writable);
  enabled_policy.begin_generation(1, now, now);
  const control::Request pending{1, 1, writable[0].key, true, now, now + 1s};
  CHECK(enabled_policy.submit(pending, now).status == control::Status::accepted);
  CHECK(!enabled_policy.read(writable[0].key, now));
  // Writable policy still does not grant the executor its interlock permission.
  CHECK(!enabled_policy.dispatch(1, now));
  CHECK(enabled_policy.snapshot(1, now)->reason == control::Reason::interlock);
  CHECK(!enabled_policy.read(writable[0].key, now));
}

struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    const auto nonce = Clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      path = std::filesystem::temp_directory_path() /
             ("headunit-control-config-" + std::to_string(nonce) + '-' + std::to_string(attempt));
      if (std::filesystem::create_directory(path)) return;
    }
    throw std::runtime_error("cannot create test configuration directory");
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void bounded_file_loading_and_disabled_example() {
  TemporaryDirectory directory;
  const auto path = directory.path / "controls.json";
  const auto write = [&](std::string_view bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(file.good());
  };
  const auto valid = document(boolean());
  write(valid);
  CHECK(control::load_configuration(path) == control::parse_configuration(valid));
  write(valid + std::string(65536 - valid.size(), ' '));
  CHECK(control::load_configuration(path).size() == 1);
  write(valid + std::string(65537 - valid.size(), ' '));
  rejects([&] { control::load_configuration(path); });
  write("");
  rejects([&] { control::load_configuration(path); });
  bool failed = false;
  try {
    control::load_configuration(directory.path / "absent.json");
  } catch (const std::runtime_error&) {
    failed = true;
  }
  CHECK(failed);
  failed = false;
  try {
    control::load_configuration(directory.path);
  } catch (const std::runtime_error&) {
    failed = true;
  }
  CHECK(failed);
  const auto example = control::load_configuration(std::filesystem::path(HEADUNIT_SOURCE_DIR) /
                                                   "config/vehicle-controls.example.json");
  CHECK(example.size() == 3);
  for (const auto& policy : example) {
    CHECK(!policy.writable && control::valid_policy(policy));
    CHECK(control::key_text(policy.key).starts_with("bench."));
  }
}

}  // namespace

int main() try {
  policy_types_defaults_and_units();
  exact_schema_and_types();
  duplicate_and_unknown_keys();
  semantic_key_limits();
  target_count_and_age_limits();
  numeric_bounds_and_exact_integer_limits();
  request_grid_and_confirmed_precision();
  malformed_json_and_size_limit();
  configuration_cannot_declare_actual_state_or_enable_backend();
  bounded_file_loading_and_disabled_example();
  std::cout << "Vehicle controls configuration: 10 focused cases passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
