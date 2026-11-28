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
#include <list>
#include <ranges>
#include <utility>

#include "utils/db_aware_allocator.hpp"
#include "utils/small_vector.hpp"

namespace memgraph::storage {

/// A sequence of small elements that is appended to by one thread and then handed to another whole.
/// Elements are held in batches, so the handover moves batches rather than elements and stays
/// constant time, while the elements of a batch sit together and cost one allocation between them.
///
/// A batch stops growing at a size worth allocating on its own, so handing over a few elements
/// carries a few elements rather than an empty page. The batch keeps its first elements inside
/// itself, which is what stops a handover of one element costing more than holding that element
/// in a list would have.
template <typename T>
class BatchedList {
 public:
  using Batch = utils::small_vector<T, memory::DbAwareAllocator<T>>;

  // Handing over a single element is the one shape a batch cannot improve on, and it only breaks
  // even because that element is held inside the batch rather than in an allocation of its own.
  // A batch keeps elements inline only while they are no larger than a pointer.
  static_assert(Batch::kSmallCapacity >= 1,
                "an element this large would make a batch of one cost more than holding it alone");

  /// Chosen so a full batch is the smallest allocation jemalloc serves from its large size classes,
  /// which is also where a growing vector stops doubling into unused space.
  static constexpr std::size_t kBatchBytes = 16UL * 1024UL;
  static constexpr std::size_t kBatchCapacity = std::max(std::size_t{1}, kBatchBytes / sizeof(T));

  BatchedList() = default;
  BatchedList(const BatchedList &) = default;
  BatchedList &operator=(const BatchedList &) = default;

  BatchedList(BatchedList &&other) noexcept
      : batches_(std::move(other.batches_)), size_(std::exchange(other.size_, 0)), fill_it_(batches_.end()) {
    other.fill_it_ = other.batches_.end();
  }

  BatchedList &operator=(BatchedList &&other) noexcept {
    if (this != &other) {
      batches_ = std::move(other.batches_);
      size_ = std::exchange(other.size_, 0);
      fill_it_ = batches_.end();
      other.fill_it_ = other.batches_.end();
    }
    return *this;
  }

  /// Pre-allocates batch nodes and element storage for at least `count` elements so subsequent
  /// `push_back` calls up to `count` require no heap allocation.
  void reserve_elements(std::size_t count) {
    std::size_t available = 0;
    for (const auto &b : batches_) {
      available += (b.capacity() > b.size()) ? (b.capacity() - b.size()) : 0;
    }
    while (available < count) {
      const std::size_t chunk = std::min(count - available, kBatchCapacity);
      batches_.emplace_back();
      batches_.back().reserve(chunk);
      available += batches_.back().capacity();
    }
    fill_it_ = batches_.begin();
    while (fill_it_ != batches_.end() && fill_it_->size() >= fill_it_->capacity()) {
      ++fill_it_;
    }
  }

  void push_back(T value) {
    if (fill_it_ != batches_.end()) {
      while (fill_it_ != batches_.end() && fill_it_->size() >= fill_it_->capacity()) {
        ++fill_it_;
      }
      if (fill_it_ != batches_.end()) {
        fill_it_->push_back(std::move(value));
        ++size_;
        return;
      }
    }
    if (batches_.empty() || batches_.back().size() >= kBatchCapacity) batches_.emplace_back();
    batches_.back().push_back(std::move(value));
    ++size_;
  }

  /// Constant time whatever the two hold: the elements are not touched, only the batches holding
  /// them change hands. `other` is left empty.
  void splice(BatchedList &other) {
    if (this == &other) return;
    while (!other.batches_.empty() && other.batches_.back().empty()) {
      other.batches_.pop_back();
    }
    batches_.splice(batches_.end(), other.batches_);
    size_ += std::exchange(other.size_, 0);
    other.fill_it_ = other.batches_.end();
  }

  void swap(BatchedList &other) noexcept {
    batches_.swap(other.batches_);
    std::swap(size_, other.size_);
    fill_it_ = batches_.end();
    other.fill_it_ = other.batches_.end();
  }

  void clear() {
    batches_.clear();
    size_ = 0;
    fill_it_ = batches_.end();
  }

  [[nodiscard]] bool empty() const { return size_ == 0; }

  [[nodiscard]] std::size_t size() const { return size_; }

  /// The elements, batch boundaries flattened away.
  [[nodiscard]] auto elements() { return batches_ | std::views::join; }

  [[nodiscard]] auto elements() const { return batches_ | std::views::join; }

  [[nodiscard]] std::size_t batch_count() const { return batches_.size(); }

 private:
  using BatchList = std::list<Batch, memory::DbAwareAllocator<Batch>>;
  BatchList batches_;
  std::size_t size_{0};
  typename BatchList::iterator fill_it_{batches_.end()};
};

}  // namespace memgraph::storage
