// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <headunit/greeting_sequence.hpp>
#include <headunit/theme/scene.hpp>
namespace headunit::theme::graphics {
// Transparent centre decoration. Instruments, data and warning paths keep
// running. Returns a new immutable scene; no signed resource is changed.
std::shared_ptr<const Layer> greeting_overlay(const GreetingFrame&, Size);
}  // namespace headunit::theme::graphics
