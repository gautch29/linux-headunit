// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#pragma once
#include <cstddef>

// Single capture window for the allocation-free display-owner regression.
namespace test_allocations {
void begin() noexcept;
std::size_t end() noexcept;
}  // namespace test_allocations
