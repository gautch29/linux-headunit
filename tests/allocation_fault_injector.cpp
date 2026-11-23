// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <cstdlib>
#include <new>

namespace {
// Only linked into single-threaded allocation fault executables.
int allocations_before_failure = -1;
}  // namespace

void fail_allocation_after(int count) {
  allocations_before_failure = count;
}

void* operator new(std::size_t size) {
  if (allocations_before_failure == 0) {
    allocations_before_failure = -1;
    throw std::bad_alloc();
  }
  if (allocations_before_failure > 0) --allocations_before_failure;
  if (auto* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}

void operator delete(void* value) noexcept {
  std::free(value);
}

void operator delete(void* value, std::size_t) noexcept {
  std::free(value);
}
