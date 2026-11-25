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

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "utils/rw_lock.hpp"

namespace memgraph::utils {

template <typename T, typename Lock>
class ConcurrentLockedValue {
 public:
  ConcurrentLockedValue(T *ptr, Lock lock) noexcept : ptr_{ptr}, lock_{std::move(lock)} {}

  ConcurrentLockedValue(const ConcurrentLockedValue &) = delete;
  ConcurrentLockedValue &operator=(const ConcurrentLockedValue &) = delete;
  ConcurrentLockedValue(ConcurrentLockedValue &&) noexcept = default;
  ConcurrentLockedValue &operator=(ConcurrentLockedValue &&) noexcept = default;

  T &get() const noexcept { return *ptr_; }
  T &operator*() const noexcept { return *ptr_; }
  T *operator->() const noexcept { return ptr_; }

  decltype(auto) operator++() { return ++(*ptr_); }
  decltype(auto) operator--() { return --(*ptr_); }

  template <typename U>
  decltype(auto) operator+=(U &&rhs) {
    return (*ptr_) += std::forward<U>(rhs);
  }

  template <typename U>
  decltype(auto) operator-=(U &&rhs) {
    return (*ptr_) -= std::forward<U>(rhs);
  }

 private:
  T *ptr_{nullptr};
  Lock lock_;
};

template <typename V>
constexpr V &UnwrapConcurrentValue(V &val) noexcept {
  return val;
}

template <typename V, typename Lock>
constexpr V &UnwrapConcurrentValue(ConcurrentLockedValue<V, Lock> &val) noexcept {
  return val.get();
}

template <typename K, typename T, typename Hash = std::hash<K>, typename KeyEqual = std::equal_to<K>,
          typename Allocator = std::allocator<std::pair<const K, T>>>
class ConcurrentUnorderedMap {
 public:
  class LifetimeGate {
   public:
    void lock_shared() {
      std::unique_lock<std::mutex> lk{mtx_};
      cv_.wait(lk, [this] { return !exclusive_; });
      ++active_readers_;
    }

    void unlock_shared() noexcept {
      std::unique_lock<std::mutex> lk{mtx_};
      if (--active_readers_ == 0) {
        cv_.notify_all();
      }
    }

    void lock() {
      std::unique_lock<std::mutex> lk{mtx_};
      cv_.wait(lk, [this] { return !exclusive_ && active_readers_ == 0; });
      exclusive_ = true;
    }

    void unlock() noexcept {
      std::unique_lock<std::mutex> lk{mtx_};
      exclusive_ = false;
      cv_.notify_all();
    }

   private:
    std::mutex mtx_;
    std::condition_variable cv_;
    std::size_t active_readers_{0};
    bool exclusive_{false};
  };

  using LockedValue = ConcurrentLockedValue<T, std::shared_lock<LifetimeGate>>;

  template <typename TAnyKey>
  LockedValue get_or_insert(const TAnyKey &key) {
    auto life_lock = std::shared_lock<LifetimeGate>{lifetime_gate_};
    {
      auto bucket_lock = std::shared_lock{mtx_};
      auto itr = map_.find(key);
      if (itr != map_.end()) {
        return LockedValue{&itr->second, std::move(life_lock)};
      }
    }
    {
      auto bucket_lock = std::unique_lock{mtx_};
      auto existing = map_.find(key);
      if (existing != map_.end()) {
        return LockedValue{&existing->second, std::move(life_lock)};
      }
      if constexpr (std::is_same_v<K, TAnyKey>) {
        if constexpr (std::is_arithmetic_v<T>) {
          const auto &[itr, _] = map_.emplace(key, T{0});
          return LockedValue{&itr->second, std::move(life_lock)};
        } else {
          const auto &[itr, _] =
              map_.emplace(std::piecewise_construct, std::forward_as_tuple(key), std::tuple<>{});
          return LockedValue{&itr->second, std::move(life_lock)};
        }
      } else {
        static_assert(std::is_constructible_v<K, TAnyKey>);
        if constexpr (std::is_arithmetic_v<T>) {
          const auto &[itr, _] = map_.emplace(K{key}, T{0});
          return LockedValue{&itr->second, std::move(life_lock)};
        } else {
          const auto &[itr, _] =
              map_.emplace(std::piecewise_construct, std::forward_as_tuple(K{key}), std::tuple<>{});
          return LockedValue{&itr->second, std::move(life_lock)};
        }
      }
    }
  }

  template <typename TAnyKey>
  LockedValue operator[](const TAnyKey &key) {
    return get_or_insert(key);
  }

  void clear() {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto bucket_lock = std::unique_lock{mtx_};
    map_.clear();
  }

  template <typename TAnyKey>
  auto find(const TAnyKey &key) const {
    auto life_lock = std::shared_lock{lifetime_gate_};
    auto bucket_lock = std::shared_lock{mtx_};
    auto itr = map_.find(key);
    return ConstIterator{std::move(itr), std::move(life_lock), std::move(bucket_lock)};
  }

  size_t size() const {
    auto bucket_lock = std::shared_lock{mtx_};
    return map_.size();
  }

  void erase(const K &key) {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto bucket_lock = std::unique_lock{mtx_};
    map_.erase(key);
  }

  template <typename... Args>
  auto emplace(Args &&...args) {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto bucket_lock = std::unique_lock{mtx_};
    auto [itr, inserted] = map_.emplace(std::forward<Args>(args)...);
    return std::pair<Iterator, bool>{Iterator{std::move(itr), std::move(life_lock), std::move(bucket_lock)}, inserted};
  }

  template <typename F>
  auto erase_if(F &&f) {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto bucket_lock = std::unique_lock{mtx_};
    return std::erase_if(map_, std::forward<F>(f));
  }

  template <typename F>
  void for_each(F &&f) const {
    auto life_lock = std::shared_lock{lifetime_gate_};
    auto bucket_lock = std::shared_lock{mtx_};
    for (const auto &[k, v] : map_) {
      f(k, v);
    }
  }

  template <typename F>
  void for_each(F &&f) {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto bucket_lock = std::unique_lock{mtx_};
    for (auto &[k, v] : map_) {
      f(k, v);
    }
  }

  struct Iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using reference = value_type &;
    using pointer = value_type *;
    using difference_type = std::ptrdiff_t;

    Iterator(typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::iterator itr,
             std::unique_lock<LifetimeGate> life_lock,
             std::unique_lock<ReadPrioritizedRWLock> lock)
        : itr_{std::move(itr)}, life_lock_{std::move(life_lock)}, lock_{std::move(lock)} {}

    Iterator(typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::iterator itr) : itr_{std::move(itr)} {}

    auto operator++() -> Iterator & {
      ++itr_;
      return *this;
    }

    auto &operator*() const { return *itr_; }

    auto *operator->() const { return &*itr_; }

    friend bool operator==(Iterator const &lhs, Iterator const &rhs) { return lhs.itr_ == rhs.itr_; }

   private:
    typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::iterator itr_;
    std::unique_lock<LifetimeGate> life_lock_;
    std::unique_lock<ReadPrioritizedRWLock> lock_;
  };

  struct ConstIterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using reference = value_type const &;
    using pointer = value_type *;
    using difference_type = std::ptrdiff_t;

    ConstIterator(typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::const_iterator itr,
                  std::shared_lock<LifetimeGate> life_lock,
                  std::shared_lock<ReadPrioritizedRWLock> lock)
        : itr_{std::move(itr)}, life_lock_{std::move(life_lock)}, lock_{std::move(lock)} {}

    ConstIterator(typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::const_iterator itr)
        : itr_{std::move(itr)} {}

    auto operator++() -> ConstIterator & {
      ++itr_;
      return *this;
    }

    const auto &operator*() const { return *itr_; }

    const auto *operator->() const { return &*itr_; }

    friend bool operator==(ConstIterator const &lhs, ConstIterator const &rhs) { return lhs.itr_ == rhs.itr_; }

   private:
    typename std::unordered_map<K, T, Hash, KeyEqual, Allocator>::const_iterator itr_;
    std::shared_lock<LifetimeGate> life_lock_;
    std::shared_lock<ReadPrioritizedRWLock> lock_;
  };

  ConstIterator begin() const {
    auto life_lock = std::shared_lock{lifetime_gate_};
    auto lock = std::shared_lock{mtx_};
    return ConstIterator{map_.begin(), std::move(life_lock), std::move(lock)};
  }

  ConstIterator end() const { return ConstIterator{map_.end()}; }

  Iterator begin() {
    auto life_lock = std::unique_lock{lifetime_gate_};
    auto lock = std::unique_lock{mtx_};
    return Iterator{map_.begin(), std::move(life_lock), std::move(lock)};
  }

  Iterator end() { return Iterator{map_.end()}; }

 private:
  std::unordered_map<K, T, Hash, KeyEqual, Allocator> map_;
  mutable LifetimeGate lifetime_gate_;
  mutable ReadPrioritizedRWLock mtx_;
};

}  // namespace memgraph::utils
