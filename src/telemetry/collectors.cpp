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

#include "telemetry/collectors.hpp"

#include <fmt/format.h>
#include <sys/types.h>
#include <unistd.h>
#include <charconv>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "utils/file.hpp"
#include "utils/stat.hpp"
#include "utils/string.hpp"

namespace memgraph::telemetry {

std::optional<std::pair<std::string, uint64_t>> ParseProcStatCpuTicks(std::string_view raw_stat) {
  const auto open_paren = raw_stat.find('(');
  const auto close_paren = raw_stat.rfind(')');
  if (open_paren == std::string_view::npos || close_paren == std::string_view::npos || close_paren <= open_paren) {
    return std::nullopt;
  }

  std::string name{raw_stat.substr(open_paren + 1, close_paren - open_paren - 1)};
  std::string_view rest = raw_stat.substr(close_paren + 1);

  std::vector<std::string_view> fields;
  fields.reserve(24);
  size_t pos = 0;
  while (pos < rest.size()) {
    while (pos < rest.size() && (rest[pos] == ' ' || rest[pos] == '\t' || rest[pos] == '\n' || rest[pos] == '\r')) {
      ++pos;
    }
    if (pos >= rest.size()) {
      break;
    }
    const size_t start = pos;
    while (pos < rest.size() && rest[pos] != ' ' && rest[pos] != '\t' && rest[pos] != '\n' && rest[pos] != '\r') {
      ++pos;
    }
    fields.push_back(rest.substr(start, pos - start));
  }

  // Fields after `(comm)` start at field 3 (`state`); field 20 (`num_threads`) is index 17,
  // and `utime`, `stime`, `cutime`, `cstime` (fields 14..17) sit at indices 11..14.
  if (fields.size() < 18) {
    return std::nullopt;
  }

  uint64_t total_ticks = 0;
  for (size_t idx = 11; idx <= 12; ++idx) {
    uint64_t val = 0;
    const auto [ptr, ec] = std::from_chars(fields[idx].data(), fields[idx].data() + fields[idx].size(), val);
    if (ec != std::errc{} || ptr != fields[idx].data() + fields[idx].size()) {
      return std::nullopt;
    }
    total_ticks += val;
  }
  for (size_t idx = 13; idx <= 14; ++idx) {
    int64_t signed_val = 0;
    const auto [ptr, ec] =
        std::from_chars(fields[idx].data(), fields[idx].data() + fields[idx].size(), signed_val);
    if (ec != std::errc{} || ptr != fields[idx].data() + fields[idx].size()) {
      return std::nullopt;
    }
    if (signed_val > 0) {
      total_ticks += static_cast<uint64_t>(signed_val);
    }
  }

  return std::make_pair(std::move(name), total_ticks);
}

namespace {
std::optional<std::pair<std::string, double>> GetCpuUsage(pid_t pid, pid_t tid = 0) {
  const std::string path =
      tid == 0 ? fmt::format("/proc/{}/stat", pid) : fmt::format("/proc/{}/task/{}/stat", pid, tid);
  const auto stat_data = utils::ReadLines(path);
  if (stat_data.empty()) {
    return std::nullopt;
  }
  const auto parsed = ParseProcStatCpuTicks(stat_data[0]);
  if (!parsed) {
    return std::nullopt;
  }
  const long clk_tck = sysconf(_SC_CLK_TCK);
  const double cpu = clk_tck > 0 ? static_cast<double>(parsed->second) / static_cast<double>(clk_tck) : 0.0;
  return std::make_pair(parsed->first, cpu);
}
}  // namespace

const nlohmann::json GetResourceUsage(std::filesystem::path root_directory) {
  // Get PID of entire process.
  pid_t pid = getpid();

  // Get CPU usage for each thread and total usage.
  nlohmann::json cpu = nlohmann::json::object();
  cpu["threads"] = nlohmann::json::array();

  // Find all threads.
  std::error_code ec;
  std::string task_file = fmt::format("/proc/{}/task", pid);
  if (!std::filesystem::exists(task_file, ec) || ec) {
    return nlohmann::json::object();
  }
  for (std::filesystem::directory_iterator it(task_file, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string tid_str = it->path().filename().string();
    pid_t tid = 0;
    const auto [ptr, parse_ec] = std::from_chars(tid_str.data(), tid_str.data() + tid_str.size(), tid);
    if (parse_ec != std::errc{} || ptr != tid_str.data() + tid_str.size() || tid <= 0) {
      continue;
    }
    if (const auto cpu_usage = GetCpuUsage(pid, tid)) {
      cpu["threads"].push_back({{"name", cpu_usage->first}, {"usage", cpu_usage->second}});
    }
  }
  const auto cpu_total = GetCpuUsage(pid);
  cpu["usage"] = cpu_total ? cpu_total->second : 0.0;
  const auto vm_max_map_count = utils::GetVmMaxMapCount();
  return {{"cpu", cpu},
          {"memory", utils::GetMemoryRES()},
          {"disk", utils::GetDirDiskUsage(root_directory)},
          {"vm_max_map_count",
           vm_max_map_count.has_value() ? *vm_max_map_count : memgraph::utils::VM_MAX_MAP_COUNT_DEFAULT}};
}

}  // namespace memgraph::telemetry
