// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/vehicle_bench.hpp>
#include <cmath>
#include <stdexcept>

namespace headunit::theme::vehicle {
namespace {
using opack::Array;
using opack::Dictionary;
using opack::Value;
Value text(std::string_view v) {
  return Value{std::string(v)};
}
Value number(std::int64_t v) {
  return Value{v};
}
Value object(Dictionary v) {
  return Value{std::move(v)};
}
// Format tags: bool=0, uint8=1, float=9, string=10. See CLIMATE_RESEARCH.md
// for evidence and the distinction between type identifiers and local IIDs.
Value characteristic(std::int64_t iid, std::int64_t type, unsigned format, bool writable,
                     std::optional<double> low = {}, std::optional<double> high = {},
                     double step = 1) {
  Dictionary d{{text("iid"), number(iid)},          {text("type"), number(type)},
               {text("format"), number(format)},    {text("mutable"), Value{writable}},
               {text("writable"), Value{writable}}, {text("supportsInvalid"), Value{false}}};
  if (format == 10) d.emplace_back(text("maximumLength"), number(128));
  if (low) {
    d.emplace_back(text("minimumValue"), Value{*low});
    d.emplace_back(text("maximumValue"), Value{*high});
    d.emplace_back(text("stepValue"), Value{step});
  }
  return object(std::move(d));
}
Value service_definition(std::int64_t iid, std::int64_t type, Array chars) {
  return object({{text("iid"), number(iid)},
                 {text("type"), number(type)},
                 {text("characteristics"), Value{std::move(chars)}},
                 {text("controls"), Value{Array{}}}});
}
Value accessory(std::int64_t iid, std::int64_t type, Array services) {
  return object({{text("iid"), number(iid)},
                 {text("type"), number(type)},
                 {text("services"), Value{std::move(services)}}});
}
}  // namespace
BenchProfile bench_profile(std::string_view name) {
  if (name == "empty") return BenchProfile::empty;
  if (name == "climate-bench") return BenchProfile::climate;
  if (name == "settings-bench") return BenchProfile::settings;
  if (name == "bench") return BenchProfile::combined;
  throw std::invalid_argument("unknown vehicle-state research profile");
}
Bench::Bench(BenchProfile mode) {
  Array accessories;
  std::vector<SignalPolicy> policies;
  std::vector<WriteBinding> writers;
  std::map<std::int64_t, Value> fixed;
  const auto signal = [&](std::int64_t iid, std::int64_t type, unsigned format, ValueKind kind,
                          std::function<Value()> read, std::function<Value(const Value&)> write,
                          std::optional<double> low = {}, std::optional<double> high = {},
                          double step = 1) {
    const bool writable = bool(write);
    policies.push_back({iid, kind, 1000, low, high});
    readers_.emplace(iid, std::move(read));
    if (write) writers.push_back({iid, std::move(write)});
    auto c = characteristic(iid, type, format, writable, low, high, step);
    // Writable is authority; mutable also includes read-only measured values.
    for (auto& [key, value] : std::get<Dictionary>(c.data))
      if (key == text("mutable")) value = Value{true};
    return c;
  };
  const auto constant = [&](std::int64_t iid, std::int64_t type, Value value) {
    const bool is_string = std::holds_alternative<std::string>(value.data);
    policies.push_back({iid, is_string ? ValueKind::string : ValueKind::integer, 1000, {}, {}});
    fixed.emplace(iid, std::move(value));
    return characteristic(iid, type, is_string ? 10 : 1, false,
                          is_string ? std::optional<double>{} : 0,
                          is_string ? std::optional<double>{} : 255);
  };
  const auto boolean = [&](std::int64_t iid, std::int64_t type, bool& state) {
    return signal(
        iid, type, 0, ValueKind::boolean, [&state] { return Value{state}; },
        [&state](const Value& v) {
          state = std::get<bool>(v.data);
          return Value{state};
        });
  };
  if (mode == BenchProfile::climate || mode == BenchProfile::combined) {
    Array temperature;
    // CurrentTemperature is the applied setpoint in this bench, not a claimed
    // cabin sensor. Target writes update it on the next service pump.
    temperature.push_back(signal(
        102, 0x31000019, 9, ValueKind::real, [this] { return Value{target_c_}; }, {}, 16, 30, 0.5));
    temperature.push_back(signal(
        103, 0x31000017, 9, ValueKind::real, [this] { return Value{target_c_}; },
        [this](const Value& v) {
          const auto n = std::get<double>(v.data);
          if (std::abs(n * 2 - std::round(n * 2)) > 1e-7)
            throw std::invalid_argument("unsupported temperature step");
          target_c_ = n;
          return Value{target_c_};
        },
        16, 30, 0.5));
    for (auto& c : temperature)
      std::get<Dictionary>(c.data).emplace_back(text("units"), number(1082));  // Celsius.
    temperature.push_back(boolean(104, 0x30000002, temperature_on_));
    temperature.push_back(constant(105, 0x36000065, text("seat_front_left")));
    Array fan;
    fan.push_back(signal(
        112, 0x31000012, 1, ValueKind::integer, [this] { return number(fan_level_); },
        [this](const Value& v) {
          fan_level_ = static_cast<unsigned>(std::get<std::int64_t>(v.data));
          return number(fan_level_);
        },
        0, 100));
    fan.push_back(boolean(113, 0x30000002, fan_on_));
    fan.push_back(boolean(114, 0x3000005f, fan_auto_));
    fan.push_back(constant(115, 0x36000065, text("seat_front_left")));
    accessories.push_back(accessory(100, 0x01000001,
                                    {service_definition(101, 0x11000002, std::move(temperature)),
                                     service_definition(111, 0x11000007, std::move(fan))}));
  }
  if (mode == BenchProfile::settings || mode == BenchProfile::combined) {
    Array setting;
    setting.push_back(boolean(202, 0x30000002, comfort_lighting_));
    setting.push_back(constant(203, 0x30000001, text("Comfort lighting (simulation)")));
    setting.push_back(constant(204, 0x30000019, text("headunit.bench.comfortLighting")));
    setting.push_back(constant(205, 0x36000028, text("headunit.bench")));
    setting.push_back(constant(206, 0x30000003, number(0)));
    // Category 0 is deliberately omitted until its enum is established.
    Array section{constant(212, 0x30000019, text("headunit.bench")),
                  constant(213, 0x36000004, text("Headunit bench")),
                  constant(214, 0x30000003, number(0))};
    accessories.push_back(accessory(200, 0x05000001,
                                    {service_definition(201, 0x16000001, std::move(setting)),
                                     service_definition(211, 0x16000020, std::move(section))}));
  }
  service_ =
      std::make_unique<Service>(object({{text("accessories"), Value{std::move(accessories)}}}),
                                std::move(policies), std::move(fixed), std::move(writers));
}
void Bench::disconnect() {
  service_->disconnect();
  refreshed_.reset();
}
void Bench::refresh(std::uint64_t now) {
  if (refreshed_ && now < *refreshed_) throw std::invalid_argument("bench clock moved backwards");
  if (refreshed_ && now - *refreshed_ < 100) return;
  for (const auto& [iid, read] : readers_)
    service_->publish(iid, read(), now, now);
  refreshed_ = now;
}
}  // namespace headunit::theme::vehicle
