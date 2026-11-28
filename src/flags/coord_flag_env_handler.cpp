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

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "flags/coord_flag_env_handler.hpp"
#include "flags/coordination.hpp"
#include "utils/logging.hpp"
#include "utils/string.hpp"

namespace memgraph::flags {

namespace {

std::expected<int, std::string> ParsePortEnv(std::string_view var_name, std::string_view raw_value) {
  const auto trimmed = utils::Trim(raw_value);
  if (trimmed.empty()) {
    return std::unexpected{fmt::format("{} cannot be empty", var_name)};
  }
  int32_t parsed = -1;
  const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size() || parsed < 0 ||
      parsed > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
    return std::unexpected{fmt::format(
        "Expected {} to be an integer port in range [0, {}], got '{}'",
        var_name,
        std::numeric_limits<uint16_t>::max(),
        raw_value)};
  }
  return static_cast<int>(parsed);
}

std::expected<int32_t, std::string> ParseCoordinatorIdEnv(std::string_view var_name, std::string_view raw_value) {
  const auto trimmed = utils::Trim(raw_value);
  if (trimmed.empty()) {
    return std::unexpected{fmt::format("{} cannot be empty", var_name)};
  }
  int32_t parsed = -1;
  const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
  if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size() || parsed < 0 || parsed >= kUnsetCoordinatorId) {
    return std::unexpected{fmt::format(
        "Expected {} to be a non-negative coordinator ID less than {}, got '{}'",
        var_name,
        kUnsetCoordinatorId,
        raw_value)};
  }
  return parsed;
}

std::optional<std::string_view> ReadOptionalEnv(const char *env_val) {
  if (env_val == nullptr) {
    return std::nullopt;
  }
  return std::string_view{env_val};
}

}  // namespace

std::string CoordinationSetup::ToString() {
  return fmt::format(
      "management port: {}, coordinator port: {}, coordinator id: {}, nuraft_log_file: {}, "
      "coordinator_hostname: {}",
      management_port,
      coordinator_port,
      coordinator_id == kUnsetCoordinatorId ? "not set" : std::to_string(coordinator_id),
      nuraft_log_file,
      coordinator_hostname);
}

[[nodiscard]] auto CoordinationSetup::IsDataInstanceManagedByCoordinator() const -> bool {
  return management_port != 0 && coordinator_port == 0 && coordinator_id == kUnsetCoordinatorId;
}

[[nodiscard]] auto CoordinationSetup::IsCoordinator() const -> bool {
  return coordinator_id != kUnsetCoordinatorId && coordinator_port != 0 && !coordinator_hostname.empty() &&
         management_port != 0;
}

std::expected<CoordinationSetup, std::string> ParseCoordinationSetupFromEnv(const CoordinationEnvInputs &envs) {
  CoordinationSetup setup{};
  if (envs.management_port.has_value()) {
    auto port = ParsePortEnv(kMgManagementPort, *envs.management_port);
    if (!port.has_value()) return std::unexpected{std::move(port.error())};
    setup.management_port = *port;
  }
  if (envs.coordinator_port.has_value()) {
    auto port = ParsePortEnv(kMgCoordinatorPort, *envs.coordinator_port);
    if (!port.has_value()) return std::unexpected{std::move(port.error())};
    setup.coordinator_port = *port;
  }
  if (envs.coordinator_id.has_value()) {
    auto cid = ParseCoordinatorIdEnv(kMgCoordinatorId, *envs.coordinator_id);
    if (!cid.has_value()) return std::unexpected{std::move(cid.error())};
    setup.coordinator_id = *cid;
  }
  if (envs.nuraft_log_file.has_value()) {
    setup.nuraft_log_file = std::string{utils::Trim(*envs.nuraft_log_file)};
  }
  if (envs.coordinator_hostname.has_value()) {
    setup.coordinator_hostname = std::string{utils::Trim(*envs.coordinator_hostname)};
  }
  return setup;
}

auto CoordinationSetupInstance() -> CoordinationSetup & {
  static auto instance = CoordinationSetup{};
  return instance;
}

void SetFinalCoordinationSetup() {
#ifdef MG_ENTERPRISE
  const CoordinationEnvInputs env_inputs{
      .management_port = ReadOptionalEnv(std::getenv(kMgManagementPort)),
      .coordinator_port = ReadOptionalEnv(std::getenv(kMgCoordinatorPort)),
      .coordinator_id = ReadOptionalEnv(std::getenv(kMgCoordinatorId)),
      .nuraft_log_file = ReadOptionalEnv(std::getenv(kMgNuRaftLogFile)),
      .coordinator_hostname = ReadOptionalEnv(std::getenv(kMgCoordinatorHostname)),
  };

  const bool any_envs_set = env_inputs.management_port.has_value() || env_inputs.coordinator_port.has_value() ||
                            env_inputs.coordinator_id.has_value() || env_inputs.nuraft_log_file.has_value() ||
                            env_inputs.coordinator_hostname.has_value();

  auto const is_flag_set = []<typename T>(T const &flag) { return flag != T{}; };

  bool const any_flags_set = is_flag_set(FLAGS_management_port) || is_flag_set(FLAGS_coordinator_port) ||
                             FLAGS_coordinator_id != kUnsetCoordinatorId || is_flag_set(FLAGS_nuraft_log_file) ||
                             is_flag_set(FLAGS_coordinator_hostname);

  if (any_flags_set && any_envs_set) {
    spdlog::warn(
        "Both environment variables and flags are set for coordinator setup. Using environment variables as priority. "
        "Flags will be ignored.");
  }

  CoordinationSetupInstance() = [&]() {
    if (!any_flags_set && !any_envs_set) {
      return CoordinationSetup{};
    }

    if (any_envs_set) {
      spdlog::trace("Coordinator will be initialized using environment variables.");
      auto parsed = ParseCoordinationSetupFromEnv(env_inputs);
      if (!parsed.has_value()) {
        LOG_FATAL("Invalid coordinator environment configuration: {}", parsed.error());
      }
      return std::move(*parsed);
    }

    spdlog::trace("Coordinator will be initialized using flags.");
    if (FLAGS_management_port < 0 || FLAGS_management_port > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
      LOG_FATAL("Invalid --management-port={}: expected port in [0, {}]",
                FLAGS_management_port,
                std::numeric_limits<uint16_t>::max());
    }
    if (FLAGS_coordinator_port < 0 ||
        FLAGS_coordinator_port > static_cast<int32_t>(std::numeric_limits<uint16_t>::max())) {
      LOG_FATAL("Invalid --coordinator-port={}: expected port in [0, {}]",
                FLAGS_coordinator_port,
                std::numeric_limits<uint16_t>::max());
    }
    if (FLAGS_coordinator_id < 0) {
      LOG_FATAL("Invalid --coordinator-id={}: expected non-negative coordinator ID", FLAGS_coordinator_id);
    }
    return CoordinationSetup{.management_port = FLAGS_management_port,
                             .coordinator_port = FLAGS_coordinator_port,
                             .coordinator_id = FLAGS_coordinator_id,
                             .nuraft_log_file = std::string{utils::Trim(FLAGS_nuraft_log_file)},
                             .coordinator_hostname = std::string{utils::Trim(FLAGS_coordinator_hostname)}};
  }();
#endif
}

}  // namespace memgraph::flags
