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
#include "flags/logging.hpp"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "gflags/gflags.h"
#include "spdlog/async.h"
#include "spdlog/async_logger.h"
#include "spdlog/common.h"
#include "spdlog/logger.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/dist_sink.h"
#include "spdlog/spdlog.h"

#include "flags/run_time_configurable.hpp"
#include "utils/enum.hpp"
#include "utils/exit_codes.hpp"
#include "utils/file.hpp"
#include "utils/file_owner.hpp"
#include "utils/flag_validation.hpp"
#include "utils/logging.hpp"
#include "utils/startup_failure.hpp"
#include "utils/string.hpp"

using namespace std::string_view_literals;

namespace {

constexpr auto kSync = "sync";
constexpr auto kAsync = "async";

spdlog::level::level_enum ParseLogLevel() {
  std::string ll;
  gflags::GetCommandLineOption("log_level", &ll);
  const auto log_level = memgraph::flags::LogLevelToEnum(ll);
  MG_ASSERT(log_level, "Invalid log level");
  return *log_level;
}

// Owner diagnosis for a failed log-file open: stat the file itself, falling
// back to its parent directory (the file usually doesn't exist yet).  The log
// path needs write access, not ownership, so the advice is the loose variant.
std::optional<std::string> LogFileOwnerHint(std::filesystem::path const &log_file) {
  namespace mu = memgraph::utils;
  auto checked = log_file;
  auto owner = mu::OwnerOf(checked);
  if (!owner) {
    checked = log_file.parent_path();
    owner = mu::OwnerOf(checked);
    if (!owner) return std::nullopt;
  }
  auto const process_euid = geteuid();
  auto const fact = mu::OwnerMismatchFact(checked, process_euid, *owner);
  if (!fact) return std::nullopt;
  return fmt::format("{} Make it writable for {} (chown/chmod), or run the process as {}.",
                     *fact,
                     mu::UsernameFor(process_euid),
                     mu::UsernameFor(*owner));
}
}  // namespace

// Logging flags
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_string(log_file, "", "Path to where the log should be stored.");

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_string(logger_type, "sync",
                        "Controls whether synchronous or asynchronous logger will be used. Options: sync, async", {
                          auto const logger_lower = memgraph::utils::ToLowerCase(memgraph::utils::Trim(value));
                          if (logger_lower != kSync && logger_lower != kAsync) {
                            std::cerr << "Expected --" << flagname << " to be 'sync' or 'async' string\n";
                            return false;
                          }
                          return true;
                        });

// default set to 35 days
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_uint64(log_retention_days, 35, "Controls for how many days will daily log files be preserved.",
                        FLAG_IN_RANGE(1, std::numeric_limits<uint16_t>::max()));

inline constexpr std::array log_level_mappings{std::pair{"TRACE"sv, spdlog::level::trace},
                                               std::pair{"DEBUG"sv, spdlog::level::debug},
                                               std::pair{"INFO"sv, spdlog::level::info},
                                               std::pair{"WARNING"sv, spdlog::level::warn},
                                               std::pair{"ERROR"sv, spdlog::level::err},
                                               std::pair{"CRITICAL"sv, spdlog::level::critical}};

namespace memgraph::flags {

namespace {

std::pair<std::string_view, std::string_view> SplitLogFilenameByExtension(std::string_view fname) {
  const auto ext_index = fname.rfind('.');
  if (ext_index == std::string_view::npos || ext_index == 0 || ext_index == fname.size() - 1) {
    return {fname, std::string_view{}};
  }
  return {fname.substr(0, ext_index), fname.substr(ext_index)};
}

bool IsValidIsoDateComponent(std::string_view date_part) {
  if (date_part.size() != 10 || date_part[4] != '-' || date_part[7] != '-') {
    return false;
  }
  for (size_t idx : {0UZ, 1UZ, 2UZ, 3UZ, 5UZ, 6UZ, 8UZ, 9UZ}) {
    if (date_part[idx] < '0' || date_part[idx] > '9') {
      return false;
    }
  }
  const int month = (date_part[5] - '0') * 10 + (date_part[6] - '0');
  const int day = (date_part[8] - '0') * 10 + (date_part[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

}  // namespace

auto LogRetentionDays() -> uint64_t { return FLAGS_log_retention_days; }

bool IsAsyncLoggerType(std::string_view logger_type) {
  return memgraph::utils::ToLowerCase(memgraph::utils::Trim(logger_type)) == kAsync;
}

bool IsRotatedLogFileFor(const std::filesystem::path &candidate_path,
                         const std::filesystem::path &configured_log_path) {
  const auto configured_name = configured_log_path.filename().string();
  const auto candidate_name = candidate_path.filename().string();
  if (configured_name.empty() || candidate_name.empty()) {
    return false;
  }
  if (candidate_path.has_parent_path() && configured_log_path.has_parent_path() &&
      candidate_path.parent_path() != configured_log_path.parent_path()) {
    return false;
  }
  const auto [base, ext] = SplitLogFilenameByExtension(configured_name);
  const size_t expected_len = base.size() + 1 + 10 + ext.size();
  if (candidate_name.size() != expected_len) {
    return false;
  }
  const std::string_view cand_sv{candidate_name};
  if (!cand_sv.starts_with(base) || cand_sv[base.size()] != '_' || !cand_sv.ends_with(ext)) {
    return false;
  }
  const auto date_part = cand_sv.substr(base.size() + 1, 10);
  return IsValidIsoDateComponent(date_part);
}

const std::string &GetAllowedLogLevels() {
  static const std::string allowed_levels = memgraph::utils::GetAllowedEnumValuesString(log_level_mappings);
  return allowed_levels;
}

bool ValidLogLevel(std::string_view value) {
  if (const auto result = memgraph::utils::IsValidEnumValueString(value, log_level_mappings); !result.has_value()) {
    const auto error = result.error();
    switch (error) {
      case memgraph::utils::ValidationError::EmptyValue: {
        std::cerr << "Log level cannot be empty." << '\n';
        break;
      }
      case memgraph::utils::ValidationError::InvalidValue: {
        std::cerr << "Invalid value for log level. Allowed values: " << GetAllowedLogLevels() << '\n';
        break;
      }
    }
    return false;
  }

  return true;
}

std::optional<spdlog::level::level_enum> LogLevelToEnum(std::string_view value) {
  return memgraph::utils::StringToEnum<spdlog::level::level_enum>(value, log_level_mappings);
}

auto GetSinkLocalTime() -> tm {
  time_t current_time{0};
  (void)time(&current_time);
  tm local_time{};
  localtime_r(&current_time, &local_time);  // localtime_r is thread-safe, unlike localtime
  return local_time;
}

// We use dist_sink which is MT safe together with _st subsinks
// This allows us MT safe
void InitializeLogger() {
  // stderr subsink
  stderr_sink()->set_level(run_time::GetAlsoLogToStderr() ? spdlog::level::trace : spdlog::level::off);

  std::vector<spdlog::sink_ptr> sub_sinks;
  sub_sinks.emplace_back(stderr_sink());

  if (!FLAGS_log_file.empty()) {
    auto const local_time = GetSinkLocalTime();
    try {
      sub_sinks.emplace_back(std::make_shared<spdlog::sinks::daily_file_sink_st>(
          FLAGS_log_file, local_time.tm_hour, local_time.tm_min, false, LogRetentionDays()));
    } catch (spdlog::spdlog_ex const &e) {
      auto message = fmt::format("Failed to open log file '{}': {}.", FLAGS_log_file, e.what());
      if (auto const hint = LogFileOwnerHint(FLAGS_log_file)) {
        message += ' ';
        message += *hint;
      }
      utils::FailStartup(utils::ExitCode::LogFileNotWritable, message);
    }
  }

  auto dist_sink = std::make_shared<spdlog::sinks::dist_sink_mt>(std::move(sub_sinks));

  FLAGS_logger_type = memgraph::utils::ToLowerCase(memgraph::utils::Trim(FLAGS_logger_type));
  const bool use_async_logger = IsAsyncLoggerType(FLAGS_logger_type);
  auto logger = std::invoke(
      [dist_sink_local = std::move(dist_sink), use_async_logger]() mutable -> std::shared_ptr<spdlog::logger> {
        if (use_async_logger) {
          // 8k size of the buffer
          spdlog::init_thread_pool(8192, 1);
          return std::make_shared<spdlog::async_logger>("memgraph_log",
                                                        std::move(dist_sink_local),
                                                        spdlog::thread_pool(),
                                                        spdlog::async_overflow_policy::overrun_oldest);
        } else {
          return std::make_shared<spdlog::logger>("memgraph_log", std::move(dist_sink_local));
        }
      });

  logger->set_level(ParseLogLevel());
  logger->flush_on(spdlog::level::trace);
  spdlog::set_default_logger(std::move(logger));
}

// This is thread-safe now because add_sink takes a lock from base_sink before adding subsink
// Main thread can execute this while the other thread is logging
void AddLoggerSink(spdlog::sink_ptr new_sink) {
  auto default_logger = spdlog::default_logger();
  auto dist_sink = std::dynamic_pointer_cast<spdlog::sinks::dist_sink_mt>(*(default_logger->sinks().begin()));
  dist_sink->add_sink(std::move(new_sink));
}

// Thread-safe because the level enum is an atomic
void TurnOffStdErr() { stderr_sink()->set_level(spdlog::level::off); }

// Thread-safe because the level enum is an atomic
// Sets log-level to trace
// Filtering done on logger's level
void TurnOnStdErr() {
  // stderr level allows everything, will be filtered on logger's elvel
  stderr_sink()->set_level(spdlog::level::trace);
}

// Deletes old log files that should've been rotated by spdlog but aren't. Spdlog saves next rotation time in the memory
// so if the instance restarts before the scheduled rotations the rotation event will never trigger. Therefore, we are
// trying to fix this behavior by running manually rotation on the Memgraph startup
// Assumes there are only log files in the directory.
// Deletes all files whose last_write_time is older than --log-retention-days
void CleanLogsDir() {
  if (FLAGS_log_file.empty()) return;

  auto const log_path = std::filesystem::path{FLAGS_log_file};
  if (!log_path.has_filename()) return;
  auto const log_directory = log_path.has_parent_path() ? log_path.parent_path() : std::filesystem::path{"."};
  auto const cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::days(LogRetentionDays());

  // Logs error only at the end, doesn't log for each file
  std::error_code ec;
  for (auto const &entry : std::filesystem::directory_iterator(log_directory, ec)) {
    std::error_code entry_ec;
    if (entry.is_symlink(entry_ec) || entry_ec) continue;
    if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
    if (!IsRotatedLogFileFor(entry.path(), log_path)) continue;
    const auto mtime = entry.last_write_time(entry_ec);
    if (!entry_ec && mtime < cutoff) {
      memgraph::utils::DeleteFile(entry.path());
    }
  }
  if (ec) {
    spdlog::warn("Error occurred while trying to manually rotate old log files: {}", ec.message());
  }
}

}  // namespace memgraph::flags
