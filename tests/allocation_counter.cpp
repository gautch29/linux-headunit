// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include "allocation_counter.hpp"
#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<bool> active{false};
std::atomic<std::size_t> allocations{0};
}  // namespace
namespace test_allocations {
void begin() noexcept {
  allocations.store(0, std::memory_order_relaxed);
  active.store(true, std::memory_order_relaxed);
}
std::size_t end() noexcept {
  active.store(false, std::memory_order_relaxed);
  return allocations.load(std::memory_order_relaxed);
}
}  // namespace test_allocations
// Keep replacement allocation/deallocation together in their own translation
// unit: optimized callers must see the matching C++ operators, not an inlined
// free() paired with an opaque new(). No compiler warning is suppressed.
void* operator new(std::size_t bytes) {
  if (active.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
  if (auto* result = std::malloc(bytes ? bytes : 1)) return result;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  ::operator delete(pointer);
}
