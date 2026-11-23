// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <utility>

#if USE_JEMALLOC
#include <jemalloc/jemalloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "utils/memory_tracker.hpp"

#if !__has_feature(thread_sanitizer)

namespace {
[[noreturn]] inline void ThrowAllocationFailure() {
  [[maybe_unused]] auto blocker = memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker{};
  auto maybe_msg = memgraph::utils::MemoryErrorStatus().msg();
  if (maybe_msg) {
    throw memgraph::utils::OutOfMemoryException{std::move(*maybe_msg)};
  }
  throw std::bad_alloc{};
}

#if !USE_JEMALLOC
inline std::size_t PlatformMallocUsableSize(void *ptr) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return 0;
  }
#if defined(__APPLE__)
  return malloc_size(ptr);
#else
  return malloc_usable_size(ptr);
#endif
}

inline void *AllocRaw(const std::size_t size, const std::optional<std::align_val_t> align) noexcept {
  if (!align.has_value()) {
    return malloc(size);
  }
  const std::size_t a = static_cast<std::size_t>(*align);
  if (a < sizeof(void *)) {
    return malloc(size);
  }
  const std::size_t rounded = (size + a - 1) & ~(a - 1);
  return aligned_alloc(a, rounded);
}
#endif

inline void *newImpl(const std::size_t size, const std::optional<std::align_val_t> align = std::nullopt) {
#if USE_JEMALLOC
  auto *ptr = [size, align] {
    const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
    return align ? aligned_alloc(static_cast<std::size_t>(*align), size) : malloc(size);
  }();
  if (ptr != nullptr) [[likely]] {
    return ptr;
  }
  ThrowAllocationFailure();
#else
  const bool accepted = [size] {
    const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
    return memgraph::utils::graph_memory_tracker.Alloc(static_cast<int64_t>(size));
  }();
  if (!accepted) [[unlikely]] {
    ThrowAllocationFailure();
  }

  void *ptr = AllocRaw(size, align);
  if (ptr == nullptr) [[unlikely]] {
    memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size));
    ThrowAllocationFailure();
  }

  const std::size_t usable = PlatformMallocUsableSize(ptr);
  if (usable > size) {
    const int64_t slack = static_cast<int64_t>(usable - size);
    const bool slack_accepted = [slack] {
      const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
      return memgraph::utils::graph_memory_tracker.Alloc(slack);
    }();
    if (!slack_accepted) [[unlikely]] {
      memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size));
      free(ptr);
      ThrowAllocationFailure();
    }
  } else if (usable < size) {
    memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size - usable));
  }
  return ptr;
#endif
}

inline void *newNoExcept(const std::size_t size,
                         const std::optional<std::align_val_t> align = std::nullopt) noexcept {
#if USE_JEMALLOC
  [[maybe_unused]] auto blocker = memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker{};
  return align ? aligned_alloc(static_cast<std::size_t>(*align), size) : malloc(size);
#else
  const bool accepted = [size]() noexcept {
    try {
      const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
      return memgraph::utils::graph_memory_tracker.Alloc(static_cast<int64_t>(size));
    } catch (...) {
      return false;
    }
  }();
  if (!accepted) [[unlikely]] {
    return nullptr;
  }

  void *ptr = [&]() noexcept {
    [[maybe_unused]] auto blocker = memgraph::utils::MemoryTracker::OutOfMemoryExceptionBlocker{};
    return AllocRaw(size, align);
  }();
  if (ptr == nullptr) [[unlikely]] {
    memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size));
    return nullptr;
  }

  const std::size_t usable = PlatformMallocUsableSize(ptr);
  if (usable > size) {
    const int64_t slack = static_cast<int64_t>(usable - size);
    const bool slack_accepted = [slack]() noexcept {
      try {
        const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
        return memgraph::utils::graph_memory_tracker.Alloc(slack);
      } catch (...) {
        return false;
      }
    }();
    if (!slack_accepted) [[unlikely]] {
      memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size));
      free(ptr);
      return nullptr;
    }
  } else if (usable < size) {
    memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(size - usable));
  }
  return ptr;
#endif
}

#if USE_JEMALLOC
extern "C" void dallocx(void *ptr, int flags);
extern "C" void sdallocx(void *ptr, size_t size, int flags);

inline void deleteImpl(void *ptr) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return;
  }

  dallocx(ptr, 0);
}

inline void deleteImpl(void *ptr, const std::align_val_t align) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return;
  }

  dallocx(ptr, MALLOCX_ALIGN(align));  // NOLINT(hicpp-signed-bitwise)
}

inline void deleteSized(void *ptr, const std::size_t size) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return;
  }

  sdallocx(ptr, size, 0);
}

inline void deleteSized(void *ptr, const std::size_t size, const std::align_val_t align) noexcept {
  if (ptr == nullptr) [[unlikely]] {
    return;
  }

  sdallocx(ptr, size, MALLOCX_ALIGN(align));  // NOLINT(hicpp-signed-bitwise)
}

#else
inline void deleteImpl(void *ptr) noexcept { free(ptr); }

inline void deleteImpl(void *ptr, const std::align_val_t /*unused*/) noexcept { free(ptr); }

inline void deleteSized(void *ptr, const std::size_t /*unused*/) noexcept { free(ptr); }

inline void deleteSized(void *ptr, const std::size_t /*unused*/, const std::align_val_t /*unused*/) noexcept {
  free(ptr);
}
#endif

inline void UntrackMemory([[maybe_unused]] void *ptr, [[maybe_unused]] std::size_t size = 0) noexcept {
  try {
#if !USE_JEMALLOC
    if (ptr != nullptr) [[likely]] {
      memgraph::utils::graph_memory_tracker.Free(static_cast<int64_t>(PlatformMallocUsableSize(ptr)));
    }
#endif
  } catch (...) {
  }
}

inline void UntrackMemory([[maybe_unused]] void *ptr, const std::align_val_t /*align*/,
                          [[maybe_unused]] std::size_t size = 0) noexcept {
  UntrackMemory(ptr, size);
}

}  // namespace

#if USE_JEMALLOC
void *JeMalloc(size_t size, int flags);

// NOLINTNEXTLINE(misc-use-internal-linkage)
__attribute__((visibility("default"))) void *JeNew(size_t size, int flags) {
  auto *ptr = [size, flags] {
    const memgraph::utils::MemoryTracker::RefusalHandledScope refusal_handled;
    return JeMalloc(size, flags);
  }();
  if (ptr != nullptr) [[likely]] {
    return ptr;
  }
  ThrowAllocationFailure();
}

void JeDealloc(void *ptr, size_t size, int flags) noexcept;

// Sized deallocation with explicit flags (e.g. MALLOCX_TCACHE_NONE).
// Mirrors JeNew: calls through JeDealloc (query tracking + je_sdallocx).
// NOLINTNEXTLINE(misc-use-internal-linkage)
__attribute__((visibility("default"))) void JeFree(void *ptr, size_t size, int flags) noexcept {
  JeDealloc(ptr, size, flags);
}
#endif

__attribute__((visibility("default"))) void *operator new(const std::size_t size) { return newImpl(size); }

__attribute__((visibility("default"))) void *operator new[](const std::size_t size) { return newImpl(size); }

__attribute__((visibility("default"))) void *operator new(const std::size_t size, const std::align_val_t align) {
  return newImpl(size, align);
}

__attribute__((visibility("default"))) void *operator new[](const std::size_t size, const std::align_val_t align) {
  return newImpl(size, align);
}

__attribute__((visibility("default"))) void *operator new(const std::size_t size,
                                                          const std::nothrow_t & /*unused*/) noexcept {
  return newNoExcept(size);
}

__attribute__((visibility("default"))) void *operator new[](const std::size_t size,
                                                            const std::nothrow_t & /*unused*/) noexcept {
  return newNoExcept(size);
}

__attribute__((visibility("default"))) void *operator new(const std::size_t size, const std::align_val_t align,
                                                          const std::nothrow_t & /*unused*/) noexcept {
  return newNoExcept(size, align);
}

__attribute__((visibility("default"))) void *operator new[](const std::size_t size, const std::align_val_t align,
                                                            const std::nothrow_t & /*unused*/) noexcept {
  return newNoExcept(size, align);
}

__attribute__((visibility("default"))) void operator delete(void *ptr) noexcept {
  UntrackMemory(ptr);
  deleteImpl(ptr);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr) noexcept {
  UntrackMemory(ptr);
  deleteImpl(ptr);
}

__attribute__((visibility("default"))) void operator delete(void *ptr, const std::align_val_t align) noexcept {
  UntrackMemory(ptr, align);
  deleteImpl(ptr, align);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr, const std::align_val_t align) noexcept {
  UntrackMemory(ptr, align);
  deleteImpl(ptr, align);
}

__attribute__((visibility("default"))) void operator delete(void *ptr, std::size_t size) noexcept {
  UntrackMemory(ptr, size);
  deleteSized(ptr, size);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr, std::size_t size) noexcept {
  UntrackMemory(ptr, size);
  deleteSized(ptr, size);
}

__attribute__((visibility("default"))) void operator delete(void *ptr, std::size_t size,
                                                            const std::align_val_t align) noexcept {
  UntrackMemory(ptr, align, size);
  deleteSized(ptr, size, align);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr, std::size_t size,
                                                              const std::align_val_t align) noexcept {
  UntrackMemory(ptr, align, size);
  deleteSized(ptr, size, align);
}

__attribute__((visibility("default"))) void operator delete(void *ptr, const std::nothrow_t & /*unused*/) noexcept {
  UntrackMemory(ptr);
  deleteImpl(ptr);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr, const std::nothrow_t & /*unused*/) noexcept {
  UntrackMemory(ptr);
  deleteImpl(ptr);
}

__attribute__((visibility("default"))) void operator delete(void *ptr, const std::align_val_t align,
                                                            const std::nothrow_t & /*unused*/) noexcept {
  UntrackMemory(ptr, align);
  deleteImpl(ptr, align);
}

__attribute__((visibility("default"))) void operator delete[](void *ptr, const std::align_val_t align,
                                                              const std::nothrow_t & /*unused*/) noexcept {
  UntrackMemory(ptr, align);
  deleteImpl(ptr, align);
}

#endif  // !__has_feature(thread_sanitizer)
