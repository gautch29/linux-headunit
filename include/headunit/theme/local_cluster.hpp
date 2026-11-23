// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/vehicle.hpp>
#include <headunit/greeting_sequence.hpp>
#include <string>
#include <functional>
namespace headunit::theme::script {
// Verified signed gauges on the existing physical background layer. Uses real
// local monotonic animation time and dated vehicle observations; never creates
// a phone clock, video producer or UI-Sync session. Exceptions retire all local
// GPU/VM objects before the caller draws its ordinary native fallback.
void run_local_cluster(const std::string& profile, headunit::VehicleSource&, unsigned width,
                       unsigned height, const std::string& output, GreetingSequenceFile* greeting,
                       const std::function<bool()>& stopping);
}  // namespace headunit::theme::script
