// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/vehicle.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace headunit;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Clock::time_point after(Clock::time_point start, double seconds) {
  return start +
         std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

double odometer(const VehicleSample& sample) {
  check(sample.odometer_km && valid_vehicle_sample(sample), "invalid simulated odometer sample");
  return *sample.odometer_km;
}

// Independently integrate the production sampler's actual speeds over a finite
// segment. Composite Simpson quadrature has a fixed cost, even at late dates.
double segment_distance(SimulatedVehicle& source, Clock::time_point start, double from, double to) {
  constexpr unsigned intervals = 4096;
  const double step = (to - from) / intervals;
  double sum = 0;
  for (unsigned i = 0; i <= intervals; ++i) {
    const auto sample = source.sample(after(start, from + step * i));
    check(sample.speed_kmh && *sample.speed_kmh >= 18 && *sample.speed_kmh <= 66,
          "existing speed curve left its unclipped range");
    const unsigned weight = i == 0 || i == intervals ? 1 : (i % 2 == 0 ? 2 : 4);
    sum += weight * *sample.speed_kmh;
  }
  return sum * step / (3 * 3600);
}

void same_sample(const VehicleSample& a, const VehicleSample& b) {
  for (const auto& reading : reading_definitions)
    check(a.*reading.member == b.*reading.member, "sample depends on polling history");
  check(a.sampled_at == b.sampled_at && a.simulated == b.simulated &&
            a.indicators == b.indicators && a.transmission == b.transmission &&
            a.drive_mode_catalog == b.drive_mode_catalog && a.drive_mode == b.drive_mode,
        "repeated source time changed fixture state");
}

void source_contract(SimulatedVehicle& source, Clock::time_point start) {
  const auto first = source.sample(start);
  check(odometer(first) == 123456 && first.speed_kmh == 42 && first.engine_rpm == 2370,
        "start baseline or original speed/RPM changed");
  check(first.fuel_percent == 62 && first.coolant_c == 88 && first.oil_c == 98 &&
            first.oil_pressure_kpa == 280 && first.battery_voltage == 14.1 &&
            first.ambient_c == 18 && first.range_km == 420,
        "other initial simulated readings changed");
  check(first.simulated && first.sampled_at == start && !first.observation_timing && !first.trip &&
            !first.heading && !first.acceleration && !first.gps_position,
        "simulation invented physical fields or observation metadata");

  const auto later = source.sample(start + 35s);
  same_sample(later, source.sample(start + 35s));
  // The existing guard rejects dates before start, not requests earlier than
  // the last poll. The exact fixture must remain independent of polling order.
  same_sample(first, source.sample(start));
  SimulatedVehicle sparse(start);
  same_sample(later, sparse.sample(start + 35s));
  bool rejected = false;
  try {
    source.sample(start - 1ns);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  check(rejected, "source accepted time before its start");
  same_sample(later, source.sample(start + 35s));

  const auto current = current_vehicle_sample(later, later.sampled_at + 500ms);
  check(current.odometer_km == later.odometer_km && current.sampled_at == later.sampled_at &&
            current.simulated,
        "current sample changed odometer/date/provenance at inclusive legacy age");
  for (const auto now : {later.sampled_at - 1ns, later.sampled_at + 500ms + 1ns}) {
    const auto unavailable = current_vehicle_sample(later, now);
    check(!unavailable.odometer_km && !unavailable.speed_kmh && !unavailable.trip &&
              unavailable.sampled_at == later.sampled_at && unavailable.simulated,
          "future/expired sample retained readings or changed original date/provenance");
  }
}

void distance_contract(SimulatedVehicle& source, Clock::time_point start) {
  const double before = odometer(source.sample(start + 20s));
  const double later = odometer(source.sample(start + 35s));
  std::cout << std::setprecision(17) << "production odometer: t=20 " << before << ", t=35 " << later
            << '\n';
  check(later >= before, "production odometer decreases between valid later source times");

  // A full period cancels the sinusoidal contribution; half a period adds
  // exactly 400 km/h*s. This oracle uses long double and actual clock dates.
  const long double period = 2 * std::numbers::pi_v<long double> / 0.12L;
  for (const auto seconds : {0.0, 1e-9, 0.001, 1.0, 20.0, 35.0, 120.0, 3600.0, 86400.0, 315576000.0,
                             double(period / 2), double(period), double(period * 1000)}) {
    const auto now = after(start, seconds);
    const long double elapsed = std::chrono::duration<long double>(now - start).count();
    const long double expected =
        123456.L + (42.L * elapsed + 200.L * (1 - std::cos(0.12L * elapsed))) / 3600.L;
    check(std::abs(odometer(source.sample(now)) - expected) < 2e-8L,
          "odometer disagrees with independent analytical distance");
  }

  for (const auto& [from, to] :
       std::array{std::pair{0.0, 20.0}, std::pair{20.0, 35.0}, std::pair{35.0, 120.0},
                  std::pair{86400.0, 86430.0}, std::pair{315576000.0, 315576030.0}}) {
    const double actual =
        odometer(source.sample(after(start, to))) - odometer(source.sample(after(start, from)));
    check(std::abs(actual - segment_distance(source, start, from, to)) < 2e-8,
          "odometer delta disagrees with finite integration of actual production speeds");
  }

  for (const double offset : {0.0, 86400.0, 315576000.0}) {
    double previous = odometer(source.sample(after(start, offset)));
    for (unsigned i = 1; i <= 9600; ++i) {
      const double distance = odometer(source.sample(after(start, offset + i * 0.125)));
      check(distance >= previous, "odometer decreased during bounded chronological sweep");
      previous = distance;
    }
  }
}
}  // namespace

int main() try {
  const auto start = headunit::Clock::time_point{std::chrono::milliseconds(1000)};
  headunit::SimulatedVehicle source(start);
  source_contract(source, start);
  distance_contract(source, start);
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
