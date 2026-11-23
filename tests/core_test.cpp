// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/backup_cluster.hpp>
#include <headunit/controller.hpp>
#include <headunit/vehicle.hpp>
#include <headunit/vehicle_wire.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* expression) {
  if (!condition) throw std::runtime_error(expression);
}
}  // namespace

#define CHECK(expression) check((expression), #expression)

int main() try {
  using namespace headunit;
  using namespace std::chrono_literals;
  const auto now = Clock::now();
  Controller controller(500ms);
  const DisplayDecision backup{};
  CHECK(controller.current() == backup);

  PhonePresentation phone{PhoneMode::ultra, true, true, now, std::nullopt};
  CHECK(controller.update(phone, now) ==
        (DisplayDecision{MainSurface::carplay, ClusterSurface::backup}));
  phone.cluster_presented_at = now;
  CHECK(controller.update(phone, now) ==
        (DisplayDecision{MainSurface::carplay, ClusterSurface::carplay}));
  CHECK(controller.update(phone, now + 501ms) == backup);
  phone.main_presented_at = now + 501ms;
  phone.cluster_presented_at = now + 501ms;
  CHECK(controller.update(phone, now + 501ms).cluster == ClusterSurface::carplay);
  phone.cluster_presented_at = now;
  CHECK(controller.update(phone, now + 501ms) ==
        (DisplayDecision{MainSurface::carplay, ClusterSurface::backup}));
  phone.cluster_presented_at = now + 501ms;
  phone.mode = PhoneMode::disconnected;
  CHECK(controller.update(phone, now + 502ms) == backup);
  phone.mode = PhoneMode::ultra;
  phone.theme_verified = false;
  CHECK(controller.update(phone, now + 502ms) == backup);
  phone.mode = PhoneMode::standard;
  CHECK(controller.update(phone, now + 502ms) ==
        (DisplayDecision{MainSurface::carplay, ClusterSurface::backup}));
  phone.authenticated = false;
  CHECK(controller.update(phone, now + 502ms) == backup);

  SimulatedVehicle vehicle(now);
  const auto first = vehicle.sample(now);
  const auto later = vehicle.sample(now + 10s);
  CHECK(first.speed_kmh && first.engine_rpm);
  CHECK(later.speed_kmh && later.engine_rpm);
  CHECK(*first.speed_kmh >= 0 && *first.engine_rpm >= 0);
  CHECK(*later.speed_kmh <= 130 && *later.engine_rpm <= 6500);
  CHECK(fresh(first, now + 100ms, 200ms));
  CHECK(!fresh(first, now + 201ms, 200ms));
  bool rejected = false;
  try {
    vehicle.sample(now - 1ms);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);

  const auto drawn = render_backup_cluster(first, now);
  const auto absent = render_backup_cluster(VehicleSample{}, now);
  const auto stale = render_backup_cluster(first, now + 1s);
  CHECK(drawn.width == 960 && drawn.height == 360);
  CHECK(drawn.pixels != absent.pixels);
  CHECK(stale.pixels == absent.pixels);
  const VehicleSample stopped{0.0, 0.0, now};
  CHECK(render_backup_cluster(stopped, now).pixels != absent.pixels);

  const auto packet = parse_vehicle_datagram("HUV1 42.5 2350\n", now);
  CHECK(packet && packet->speed_kmh && packet->engine_rpm);
  CHECK(*packet->speed_kmh == 42.5 && *packet->engine_rpm == 2350);
  CHECK(packet->sampled_at == now);
  const auto partial = parse_vehicle_datagram("HUV1 - 0", now);
  CHECK(partial && !partial->speed_kmh && partial->engine_rpm == 0);
  CHECK(!parse_vehicle_datagram("HUV1 -10 2000", now));
  CHECK(!parse_vehicle_datagram("HUV1 10 nan", now));
  CHECK(!parse_vehicle_datagram("HUV1 10 2000 extra", now));
  CHECK(!parse_vehicle_datagram("HUV2 10 2000", now));
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
