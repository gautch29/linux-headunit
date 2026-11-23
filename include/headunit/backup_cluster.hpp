// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/vehicle.hpp>

#include <cstdint>
#include <iosfwd>
#include <vector>

namespace headunit {

struct Rgb {
  std::uint8_t r, g, b;
  bool operator==(const Rgb&) const = default;
};

struct Image {
  unsigned width;
  unsigned height;
  std::vector<Rgb> pixels;
  Rgb at(unsigned x, unsigned y) const;
};

// Minimal fallback view. Missing/stale values draw dashes, never zero.
Image render_backup_cluster(const VehicleSample& sample, Clock::time_point now,
                            unsigned width = 960, unsigned height = 360);
void write_ppm(const Image& image, std::ostream& output);

}  // namespace headunit
