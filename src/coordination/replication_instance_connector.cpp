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

#ifdef MG_ENTERPRISE

#include "coordination/replication_instance_connector.hpp"

#include "replication_coordination_glue/handler.hpp"

#include <chrono>
#include <string>

namespace memgraph::coordination {

namespace {
auto SteadyNowNs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

TimedFailureDetector::TimedFailureDetector(uint32_t const instance_down_timeout_sec)
    : instance_down_timeout_sec_(instance_down_timeout_sec), last_response_steady_ns_(SteadyNowNs()) {}

auto TimedFailureDetector::IsAlive() const -> bool { return is_alive_.load(std::memory_order_acquire); }

auto TimedFailureDetector::LastSuccRespMs() const -> std::chrono::milliseconds {
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  using std::chrono::nanoseconds;

  const auto elapsed_ns = std::max<int64_t>(0, SteadyNowNs() - last_response_steady_ns_.load(std::memory_order_acquire));
  return duration_cast<milliseconds>(nanoseconds{elapsed_ns});
}

auto TimedFailureDetector::Suspect() -> bool {
  const auto elapsed_ns = std::max<int64_t>(0, SteadyNowNs() - last_response_steady_ns_.load(std::memory_order_acquire));
  const bool alive =
      std::chrono::nanoseconds{elapsed_ns} < std::chrono::seconds{instance_down_timeout_sec_.load(std::memory_order_acquire)};
  is_alive_.store(alive, std::memory_order_release);
  return alive;
}

auto TimedFailureDetector::Restore() -> void {
  last_response_steady_ns_.store(SteadyNowNs(), std::memory_order_release);
  is_alive_.store(true, std::memory_order_release);
}

void TimedFailureDetector::UpdateInstanceDownTimeoutSec(uint32_t const new_config) {
  instance_down_timeout_sec_.store(new_config, std::memory_order_release);
}

ReplicationInstanceConnector::ReplicationInstanceConnector(
    DataInstanceConfig const &config, CoordinatorInstance *coord_instance, uint32_t const instance_down_timeout_sec,
    const std::chrono::seconds instance_health_check_frequency_sec, std::optional<utils::TlsConfig> const &tls_config)
    : client_(ReplicationInstanceClient(config.instance_name, config.mgt_server, coord_instance,
                                        instance_health_check_frequency_sec, tls_config)),
      timed_failure_detector_(instance_down_timeout_sec),
      // Stored by value to avoid a dangling pointer: callers build
      repl_client_info_(config.replication_client_info) {}

// Intentional logical constness
void ReplicationInstanceConnector::OnSuccessPing() const { timed_failure_detector_.Restore(); }

// Intentional logical constness
auto ReplicationInstanceConnector::OnFailPing() const -> bool { return timed_failure_detector_.Suspect(); }

auto ReplicationInstanceConnector::IsAlive() const -> bool { return timed_failure_detector_.IsAlive(); }

auto ReplicationInstanceConnector::LastSuccRespMs() const -> std::chrono::milliseconds {
  return timed_failure_detector_.LastSuccRespMs();
}

auto ReplicationInstanceConnector::InstanceName() const -> std::string const & { return client_.InstanceName(); }

auto ReplicationInstanceConnector::SendSwapAndUpdateUUID(utils::UUID const &new_main_uuid) const -> bool {
  return replication_coordination_glue::SendSwapMainUUIDRpc(client_.RpcClient(), new_main_uuid);
}

void ReplicationInstanceConnector::UpdateInstanceDownTimeoutSec(uint32_t const new_config) const {
  timed_failure_detector_.UpdateInstanceDownTimeoutSec(new_config);
}

void ReplicationInstanceConnector::UpdateHealthCheckFrequencySec(std::chrono::seconds const &new_config) const {
  client_.UpdateHealthCheckFrequencySec(new_config);
}

auto ReplicationInstanceConnector::StartStateCheck() -> void { client_.StartStateCheck(); }

auto ReplicationInstanceConnector::StopStateCheck() -> void { client_.StopStateCheck(); }

auto ReplicationInstanceConnector::PauseStateCheck() -> void { client_.PauseStateCheck(); }

auto ReplicationInstanceConnector::ResumeStateCheck() -> void { client_.ResumeStateCheck(); }

auto ReplicationInstanceConnector::GetClient() const -> ReplicationInstanceClient const & { return client_; }

}  // namespace memgraph::coordination

#endif
