// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once

#include <headunit/theme/vehicle_configuration.hpp>
#include <headunit/vehicle_commands.hpp>
#include <functional>

namespace headunit::theme::vehicle {

// Expose only independently observed or explicitly acknowledged actual state.
// Semantic keys and scalar types come from the Coordinator's target policies;
// the capability configuration supplies the Apple characteristics separately.
// Supply the actual owner operation time, in the Coordinator's host clock
// domain, rather than reconstructing a fine timestamp from a service tick.
using OwnerClock = std::function<Clock::time_point()>;
// Calls share the Coordinator's owner thread and host steady-clock domain.
// The nondecreasing service tick may not be ahead of the supplied owner time;
// confirmations in a later service millisecond are withheld without changing
// their ordering frontier. Invalid caller times leave bridge history intact.
// Confirmation dates are coarsened to milliseconds, never renewed by a read.
// Same-millisecond collisions remain unavailable until a newer date arrives.
// This adapter cannot synchronously confirm a physical write: apply rejects
// every request and never submits or dispatches a command.
std::shared_ptr<Provider> confirmed_state_provider(
    std::shared_ptr<headunit::vehicle_control::Coordinator> coordinator, OwnerClock owner_clock);

}  // namespace headunit::theme::vehicle
