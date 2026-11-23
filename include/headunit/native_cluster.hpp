// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/backup_cluster.hpp>
#include <headunit/indicator_symbols.hpp>
#include <headunit/route_guidance.hpp>
#include <headunit/native_cluster_configuration.hpp>
#include <headunit/greeting_sequence.hpp>
#include <cstdint>
#include <string>
namespace headunit {
struct ClusterMapRect {
  int x, y, width, height;
};
enum class ClusterCenter { navigation, media, vehicle, trip, minimal, sport, gauges };

// The native cluster is designed on one 1920 × 720 canvas, fitted without
// distortion into any output. These named regions are the single source of
// its geometry: the renderer draws inside them and tests check against them.
namespace cluster_layout {
using Region = IndicatorFrame;
inline constexpr int width = indicator_layout::width, height = indicator_layout::height;
inline constexpr Region speed{60, 96, 480, 430}, power{1380, 96, 480, 430};
inline constexpr Region drive_mode{205, 462, 190, 40}, gear{1525, 462, 190, 40};
inline constexpr Region telltales = indicator_layout::row;
// Same bottom warning slots and top direction arrows as Ultra.
// Off/dark slots stay empty; unavailable directions show a faint dashed arrow outline.
inline constexpr Region indicator_frame(Indicator signal) {
  return indicator_layout::frame(signal);
}
inline constexpr Region center{560, 80, 800, 440};
inline constexpr Region minimal{560, 96, 800, 430}, sport{480, 96, 960, 430};
inline constexpr Region bottom_bar{40, 536, 1840, 104};
inline constexpr Region media{64, 544, 600, 88}, guidance{700, 544, 560, 88},
    readings{1300, 544, 560, 88};
inline constexpr Region data_status{40, 24, 400, 32};
}  // namespace cluster_layout

// Unknown or unavailable map selections resolve to Media. A shared state file
// lets the local controls, Standard CarPlay and phone-free cluster agree.
ClusterCenter selected_cluster_center(const std::string& path, bool map_available);
// The phone's map covers the whole fitted canvas; native instruments are drawn
// above it, on a transparent layer, with scrims where they meet the map.
ClusterMapRect native_cluster_map(unsigned width, unsigned height);

struct ClusterContent {
  VehicleSample vehicle;
  std::string media_file;
  NavigationSnapshot navigation;
  ClusterCenter center = ClusterCenter::navigation;
  bool phone_connected = false;
  NativeClusterConfiguration configuration{};
  GreetingFrame greeting{};
  const TurnSignalDisplay* turn_signals = nullptr;
};
// Draws premultiplied ARGB8888 (native-endian, the wl_shm ARGB8888 layout) into
// caller-owned pixels. Map leaves its center transparent for the map surface
// beneath; every other page, and the phone-free cluster, is fully opaque.
void draw_native_cluster(const ClusterContent&, Clock::time_point, std::uint32_t* pixels,
                         unsigned width, unsigned height, std::size_t stride_bytes);
// Opaque convenience for snapshots and tests: the same frame over black.
Image render_native_cluster(const VehicleSample&, Clock::time_point, unsigned width,
                            unsigned height, const std::string& media_file = {},
                            const NavigationSnapshot& navigation = {},
                            ClusterCenter center = ClusterCenter::navigation,
                            bool phone_connected = false,
                            const NativeClusterConfiguration& configuration = {});
}  // namespace headunit
