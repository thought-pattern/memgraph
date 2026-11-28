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

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory_resource>
#include <new>
#include <utility>

namespace memgraph::utils {

/// This is a monotonic allocator which:
/// - grabs 1 page for each slab
/// - allocations are aligned to page size
/// - allocations smaller than a page use remaining space in current slab
/// - allocations larger than a page get their own allocations
/// - deallocation is a noop
/// - all memory released on destruction
///
/// An optional upstream std::pmr::memory_resource can be supplied at
/// construction. All backing page allocations go through that upstream
/// (default: operator new via std::pmr::get_default_resource()). Callers that
/// need DB-arena attribution pass an ArenaMemoryResource as the upstream.
struct PageSlabMemoryResource : std::pmr::memory_resource {
  static constexpr std::size_t PAGE_SIZE = 4096;

  explicit PageSlabMemoryResource(std::pmr::memory_resource *upstream = std::pmr::get_default_resource()) noexcept
      : upstream_(upstream) {}

  PageSlabMemoryResource(PageSlabMemoryResource const &) = delete;
  PageSlabMemoryResource &operator=(PageSlabMemoryResource const &) = delete;

  PageSlabMemoryResource(PageSlabMemoryResource &&other) noexcept
      : upstream_(other.upstream_),
        pages(std::exchange(other.pages, nullptr)),
        ptr(std::exchange(other.ptr, nullptr)),
        space(std::exchange(other.space, 0)) {}

  PageSlabMemoryResource &operator=(PageSlabMemoryResource &&other) noexcept {
    if (this == &other) return *this;
    release_pages();
    upstream_ = other.upstream_;
    pages = std::exchange(other.pages, nullptr);
    ptr = std::exchange(other.ptr, nullptr);
    space = std::exchange(other.space, 0);
    return *this;
  }

  ~PageSlabMemoryResource() override { release_pages(); }

 private:
  struct header {
    explicit header(header *next, std::size_t alloc_size, std::align_val_t alignment)
        : next(next), alloc_size{alloc_size}, alignment{alignment} {}

    header *next = nullptr;
    std::size_t alloc_size;  // total bytes passed to upstream_->allocate()
    std::align_val_t alignment;
  };

  void release_pages() noexcept {
    auto *current = std::exchange(pages, nullptr);
    ptr = nullptr;
    space = 0;
    while (current) {
      auto *next = current->next;
      upstream_->deallocate(current, current->alloc_size, static_cast<std::size_t>(current->alignment));
      current = next;
    }
  }

  constexpr static size_t alignSize(size_t size, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<size_t>::max() - (alignment - 1)) {
      throw std::bad_alloc();
    }
    return (size + alignment - 1) & ~(alignment - 1);
  }

  void *do_allocate(size_t bytes, size_t alignment) final {
    // 1. could this fit inside a page slab?
    constexpr auto header_size = sizeof(header);
    const auto effective_alignment = std::max(alignment, alignof(header));
    const auto earliest_slab_position = alignSize(header_size, effective_alignment);
    if (effective_alignment > PAGE_SIZE || earliest_slab_position >= PAGE_SIZE ||
        (PAGE_SIZE - earliest_slab_position) < bytes) [[unlikely]] {
      if (bytes > std::numeric_limits<size_t>::max() - earliest_slab_position) {
        throw std::bad_alloc();
      }
      const auto required_bytes = bytes + earliest_slab_position;
      auto *newmem = reinterpret_cast<header *>(upstream_->allocate(required_bytes, effective_alignment));
      // add to the allocation list
      pages = std::construct_at<header>(newmem, pages, required_bytes, std::align_val_t{effective_alignment});
      return reinterpret_cast<std::byte *>(pages) + earliest_slab_position;
    }

    // 2. can it fit in existing slab?
    if (!std::align(alignment, bytes, ptr, space)) [[unlikely]] {
      auto *newmem = reinterpret_cast<header *>(upstream_->allocate(PAGE_SIZE, PAGE_SIZE));
      pages = std::construct_at<header>(newmem, pages, PAGE_SIZE, std::align_val_t{PAGE_SIZE});
      ptr = reinterpret_cast<std::byte *>(pages) + header_size;
      space = PAGE_SIZE - header_size;
      if (!std::align(alignment, bytes, ptr, space)) [[unlikely]] {
        // This should never happen for reasonable alignments
        std::unreachable();
      }
    }

    // 3. use current slab
    // NOTE: ptr and space have already been updated via std::align
    void *res = ptr;
    ptr = reinterpret_cast<std::byte *>(ptr) + bytes;
    space -= bytes;
    return res;
  }

  void do_deallocate(void * /*p*/, size_t /*bytes*/, size_t /*alignment*/) final { /*noop*/ }

  bool do_is_equal(memory_resource const &other) const noexcept final { return std::addressof(other) == this; }

 public:
  void set_upstream(std::pmr::memory_resource *upstream) noexcept { upstream_ = upstream; }

 private:
  std::pmr::memory_resource *upstream_ = std::pmr::get_default_resource();
  header *pages = nullptr;
  void *ptr = nullptr;
  size_t space = 0;
};

}  // namespace memgraph::utils
