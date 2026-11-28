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

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <nlohmann/json_fwd.hpp>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace memgraph::communication {

class BoltMetrics {
 public:
  static constexpr size_t kMaxDistinctDriverEntries = 128;

  enum class ConnectionType { kAnonymous = 0, kBasic, Count };
  static constexpr std::array<std::string_view, (int)ConnectionType::Count> ct_to_str = {"anonymous", "basic"};

  static std::string ConnectionTypeStr(ConnectionType type) { return std::string(ct_to_str[(int)type]); }
  class Metrics;

  struct Info {
    explicit Info(std::string name) : name(std::move(name)) {
      for (auto &ct : connection_types) ct.store(0, std::memory_order_relaxed);
    }

    Info(std::string name, std::string bolt_v, std::vector<std::string> supported_bolt_v)
        : name(std::move(name)), bolt_v_(std::move(bolt_v)), supported_bolt_v_(std::move(supported_bolt_v)) {
      for (auto &ct : connection_types) ct.store(0, std::memory_order_relaxed);
    }

    void SetVersionInfo(std::string bolt_v, std::vector<std::string> supported_bolt_v) {
      std::lock_guard lock{version_mtx_};
      bolt_v_ = std::move(bolt_v);
      supported_bolt_v_ = std::move(supported_bolt_v);
    }

    void AbsorbCountsFrom(const Info &other) noexcept {
      for (int i = 0; i < static_cast<int>(ConnectionType::Count); ++i) {
        connection_types[i].fetch_add(other.connection_types[i].load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
      }
      sessions.fetch_add(other.sessions.load(std::memory_order_relaxed), std::memory_order_relaxed);
      queries.fetch_add(other.queries.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    [[nodiscard]] int ActiveSessions() const noexcept { return concurrent_users.load(std::memory_order_acquire); }

    const std::string name;                                        //!< Driver name
    std::atomic_int connection_types[(int)ConnectionType::Count];  //!< Authentication used when connecting
    std::atomic_int sessions{0};                                   //!< Number of sessions using the same driver
    std::atomic_int queries{0};                                    //!< Queries executed by the driver

    nlohmann::json ToJson() const;

   private:
    friend class Metrics;
    friend class BoltMetrics;
    mutable std::mutex version_mtx_;
    std::string bolt_v_;                         //!< Bolt version used
    std::vector<std::string> supported_bolt_v_;  //!< Bolt versions supported by the driver
    std::atomic_int concurrent_users{0};
  };

  class Metrics {
    friend class BoltMetrics;

    explicit Metrics(Info &info) : info_(&info) { info_->concurrent_users.fetch_add(1, std::memory_order_acq_rel); }

   public:
    ~Metrics() {
      if (info_) info_->concurrent_users.fetch_sub(1, std::memory_order_acq_rel);
    }

    Metrics(const Metrics &other) : info_(other.info_) {
      if (info_) info_->concurrent_users.fetch_add(1, std::memory_order_acq_rel);
    }

    Metrics(Metrics &&other) noexcept : info_(other.info_) { other.info_ = nullptr; }

    Metrics &operator=(const Metrics &other) {
      if (this != &other) {
        if (info_) info_->concurrent_users.fetch_sub(1, std::memory_order_acq_rel);
        info_ = other.info_;
        if (info_) info_->concurrent_users.fetch_add(1, std::memory_order_acq_rel);
      }
      return *this;
    }

    Metrics &operator=(Metrics &&other) noexcept {
      if (this != &other) {
        if (info_) info_->concurrent_users.fetch_sub(1, std::memory_order_acq_rel);
        info_ = other.info_;
        other.info_ = nullptr;  // Invalidate the source object
      }
      return *this;
    }

    Info *operator->() { return info_; }

    Info *info_;
  };

  Metrics Add(std::string name) {
    auto l = std::unique_lock{mtx};
    if (auto existing = info.find(name); existing != info.end()) {
      return Metrics(existing->second);
    }
    ReclaimInactiveIfAtCapacityLocked();
    if (info.size() >= kMaxDistinctDriverEntries) {
      return Metrics(aggregated_other_);
    }
    auto key = name;
    auto [it, _] = info.emplace(
        std::piecewise_construct, std::forward_as_tuple(std::move(key)), std::forward_as_tuple(std::move(name)));
    return Metrics(it->second);
  }

  Metrics Add(std::string name, std::string bolt_v, std::vector<std::string> supported_bolt_v) {
    auto l = std::unique_lock{mtx};
    if (auto existing = info.find(name); existing != info.end()) {
      existing->second.SetVersionInfo(std::move(bolt_v), std::move(supported_bolt_v));
      return Metrics(existing->second);
    }
    ReclaimInactiveIfAtCapacityLocked();
    if (info.size() >= kMaxDistinctDriverEntries) {
      aggregated_other_.SetVersionInfo(std::move(bolt_v), std::move(supported_bolt_v));
      return Metrics(aggregated_other_);
    }
    auto key = name;
    auto [it, _] = info.emplace(std::piecewise_construct,
                                std::forward_as_tuple(std::move(key)),
                                std::forward_as_tuple(std::move(name), std::move(bolt_v), std::move(supported_bolt_v)));
    return Metrics(it->second);
  }

  nlohmann::json ToJson();

 private:
  void ReclaimInactiveIfAtCapacityLocked() {
    if (info.size() < kMaxDistinctDriverEntries) return;
    for (auto it = info.begin(); it != info.end();) {
      if (it->second.ActiveSessions() == 0) {
        aggregated_other_.AbsorbCountsFrom(it->second);
        it = info.erase(it);
      } else {
        ++it;
      }
    }
  }

  mutable std::mutex mtx;
  std::map<std::string, Info> info;
  Info aggregated_other_{"other"};
};

extern BoltMetrics bolt_metrics;

}  // namespace memgraph::communication
