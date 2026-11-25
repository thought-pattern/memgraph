// Copyright 2025 Memgraph Ltd.
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

#include <atomic>
#include <chrono>
#include <cstdint>

namespace memgraph::utils {

// This class is threadsafe.
class Timer {
 public:
  Timer() noexcept : start_ns_(NowNs()) {}

  Timer(Timer const &other) noexcept : start_ns_(other.start_ns_.load(std::memory_order_relaxed)) {}

  Timer &operator=(Timer const &other) noexcept {
    if (this != &other) {
      start_ns_.store(other.start_ns_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    return *this;
  }

  template <typename TDuration = std::chrono::duration<double>>
  TDuration Elapsed() const noexcept {
    auto const elapsed_ns = NowNs() - start_ns_.load(std::memory_order_relaxed);
    return std::chrono::duration_cast<TDuration>(std::chrono::nanoseconds(elapsed_ns));
  }

  void ResetStartTime() noexcept { start_ns_.store(NowNs(), std::memory_order_relaxed); }

 private:
  static int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  std::atomic<int64_t> start_ns_;
};

}  // namespace memgraph::utils
