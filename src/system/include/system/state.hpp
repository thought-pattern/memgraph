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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "kvstore/kvstore.hpp"

namespace memgraph::system {

namespace {
constexpr std::string_view kLastCommitedSystemTsKey = "last_committed_system_ts";  // Key for timestamp durability
}  // namespace

struct State {
  explicit State(std::optional<std::filesystem::path> storage, bool recovery_on_startup);

  [[nodiscard]] bool PersistTimestamp(std::uint64_t timestamp) {
    if (durability_ && !durability_->Put(kLastCommitedSystemTsKey, std::to_string(timestamp))) {
      return false;
    }
    return true;
  }

  void AdvanceInMemoryTimestamp(std::uint64_t timestamp) noexcept {
    last_committed_system_timestamp_.store(timestamp, std::memory_order_release);
  }

  void FinalizeTransaction(std::uint64_t timestamp) {
    if (!PersistTimestamp(timestamp)) {
      throw kvstore::KVStoreError("Failed to persist committed system transaction timestamp {}", timestamp);
    }
    AdvanceInMemoryTimestamp(timestamp);
  }

  auto LastCommittedSystemTimestamp() const -> uint64_t {
    return last_committed_system_timestamp_.load(std::memory_order_acquire);
  }

 private:
  friend struct ReplicaHandlerAccessToState;
  friend struct Transaction;

  std::optional<kvstore::KVStore> durability_;
  std::atomic_uint64_t last_committed_system_timestamp_{};
};

struct ReplicaHandlerAccessToState {
  explicit ReplicaHandlerAccessToState(memgraph::system::State &state) : state_{&state} {}

  auto LastCommitedTS() const -> uint64_t {
    return state_->last_committed_system_timestamp_.load(std::memory_order_acquire);
  }

  bool SetLastCommitedTS(uint64_t new_timestamp) {
    if (!state_->PersistTimestamp(new_timestamp)) {
      return false;
    }
    state_->AdvanceInMemoryTimestamp(new_timestamp);
    return true;
  }

 private:
  State *state_;
};

}  // namespace memgraph::system
