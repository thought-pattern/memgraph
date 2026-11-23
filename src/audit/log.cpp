// Copyright 2026 Memgraph Ltd.
//
// Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
// License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
// this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.
//
//

#include "audit/log.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <sstream>
#include <tuple>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <utility>

#include "communication/bolt/v1/mg_types.hpp"
#include "query/string_helpers.hpp"
#include "utils/credential_redaction.hpp"
#include "utils/logging.hpp"
#include "utils/string.hpp"
#include "utils/temporal.hpp"

#include <mutex>
#include <unordered_set>

namespace memgraph::audit {

namespace {

communication::bolt::Value RedactBoltValue(const communication::bolt::Value &value) {
  using enum communication::bolt::Value::Type;
  switch (value.type()) {
    case String:
      if (utils::LooksLikePasswordHash(value.ValueString())) {
        return communication::bolt::Value{utils::kRedactedMask};
      }
      return value;
    case List: {
      std::vector<communication::bolt::Value> redacted;
      const auto &items = value.ValueList();
      redacted.reserve(items.size());
      for (const auto &item : items) {
        redacted.push_back(RedactBoltValue(item));
      }
      return communication::bolt::Value{std::move(redacted)};
    }
    case Map: {
      const auto &bolt_map = value.ValueMap();
      if (communication::bolt::BoltMapToMgTypeInfo(bolt_map)) {
        return value;
      }
      communication::bolt::map_t redacted_map;
      for (const auto &[k, v] : bolt_map) {
        if (utils::IsSensitiveParamKey(k)) {
          redacted_map.emplace(k, communication::bolt::Value{utils::kRedactedMask});
        } else {
          redacted_map.emplace(k, RedactBoltValue(v));
        }
      }
      return communication::bolt::Value{std::move(redacted_map)};
    }
    default:
      return value;
  }
}

}  // namespace

std::pair<std::string, memgraph::communication::bolt::map_t> RedactAuditQueryAndParams(
    const std::string &query, const memgraph::communication::bolt::map_t &params) {
  std::unordered_set<std::string> redacted_param_names;
  std::string redacted_query = utils::RedactQueryCredentials(query, &redacted_param_names);

  communication::bolt::map_t redacted_params;
  for (const auto &[k, v] : params) {
    if (redacted_param_names.contains(k) || utils::IsSensitiveParamKey(k)) {
      redacted_params.emplace(k, communication::bolt::Value{utils::kRedactedMask});
    } else {
      redacted_params.emplace(k, RedactBoltValue(v));
    }
  }
  return {std::move(redacted_query), std::move(redacted_params)};
}

// Helper function that converts a `communication::bolt::Value` to `nlohmann::json`.
inline nlohmann::json BoltValueToJson(const communication::bolt::Value &value) {
  nlohmann::json ret;
  switch (value.type()) {
    using enum memgraph::communication::bolt::Value::Type;
    case Null:
      break;
    case Bool:
      ret = value.ValueBool();
      break;
    case Int:
      ret = value.ValueInt();
      break;
    case Double:
      ret = value.ValueDouble();
      break;
    case String:
      ret = value.ValueString();
      break;
    case List: {
      ret = nlohmann::json::array();
      for (const auto &item : value.ValueList()) {
        ret.push_back(BoltValueToJson(item));
      }
      break;
    }
    case Map: {
      auto const &bolt_value_map = value.ValueMap();
      auto const info = memgraph::communication::bolt::BoltMapToMgTypeInfo(bolt_value_map);
      if (info) {
        switch (info->type) {
          case communication::bolt::MgType::Enum: {
            ret = info->value_str;
            break;
          }
        }
      } else {
        ret = nlohmann::json::object();
        for (const auto &[map_k, map_v] : bolt_value_map) {
          ret.push_back(nlohmann::json::object_t::value_type(map_k, BoltValueToJson(map_v)));
        }
      }
      break;
    }
    case Date: {
      std::stringstream ss;
      ss << utils::Date(std::chrono::microseconds{value.ValueDate().MicrosecondsSinceEpoch()});
      ret = ss.str();
      break;
    }
    case Duration: {
      std::stringstream ss;
      ss << utils::Duration(value.ValueDuration().microseconds);
      ret = ss.str();
      break;
    }
    case LocalTime: {
      std::stringstream ss;
      ss << utils::LocalTime(value.ValueLocalTime().MicrosecondsSinceEpoch());
      ret = ss.str();
      break;
    }
    case LocalDateTime: {
      std::stringstream ss;
      ss << value.ValueLocalDateTime();
      ret = ss.str();
      break;
    }
    case ZonedDateTime: {
      const auto &temp_value = value.ValueZonedDateTime();
      std::stringstream ss;
      ss << utils::ZonedDateTime(temp_value.SysTimeSinceEpoch(), temp_value.GetTimezone());
      ret = ss.str();
      break;
    }
    case Point2d: {
      std::stringstream ss;
      ss << query::CypherConstructionFor(value.ValuePoint2d());
      ret = ss.str();
      break;
    }
    case Point3d: {
      std::stringstream ss;
      ss << query::CypherConstructionFor(value.ValuePoint3d());
      ret = ss.str();
      break;
    }
    case Vertex:
    case Edge:
    case UnboundedEdge:
    case Path: {
      // Should not be sent for audit
      break;
    }
  }
  return ret;
}

Log::Log(std::filesystem::path storage_directory, int32_t buffer_size, int32_t buffer_flush_interval_millis)
    : storage_directory_(std::move(storage_directory)),
      buffer_size_(buffer_size),
      buffer_flush_interval_millis_(buffer_flush_interval_millis),
      started_(false) {}

bool Log::Start() {
  MG_ASSERT(!started_.load(std::memory_order_acquire), "Trying to start an already started audit log!");

  utils::EnsureDirOrDie(storage_directory_);

  buffer_.emplace(buffer_size_);
  reopen_requested_.store(false, std::memory_order_relaxed);
  stop_reopen_.store(false, std::memory_order_relaxed);

  {
    auto guard = std::lock_guard{lock_};
    if (!log_.Open(storage_directory_ / "audit.log", utils::OutputFile::Mode::APPEND_TO_EXISTING)) {
      return false;
    }
  }

  if (::pipe(reopen_pipe_) != 0) {
    auto guard = std::lock_guard{lock_};
    if (log_.IsOpen()) log_.Close();
    return false;
  }
  for (int fd : reopen_pipe_) {
    int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags >= 0) {
      (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    int status_flags = ::fcntl(fd, F_GETFL);
    if (status_flags >= 0) {
      (void)::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK);
    }
  }

  started_.store(true, std::memory_order_release);
  reopen_thread_ = std::thread([this] { ReopenLoop(); });
  scheduler_.SetInterval(std::chrono::milliseconds(buffer_flush_interval_millis_));
  scheduler_.Run("Audit", [&] { Flush(); });
  return true;
}

Log::~Log() {
  if (!started_.load(std::memory_order_acquire)) return;

  started_.store(false, std::memory_order_release);
  stop_reopen_.store(true, std::memory_order_release);
  if (reopen_pipe_[1] >= 0) {
    const char wake = 1;
    [[maybe_unused]] const auto written = ::write(reopen_pipe_[1], &wake, 1);
  }
  if (reopen_thread_.joinable()) {
    reopen_thread_.join();
  }
  CloseReopenPipe();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  scheduler_.Stop();
  Flush();
}

void Log::CloseReopenPipe() noexcept {
  for (int &fd : reopen_pipe_) {
    if (fd >= 0) {
      (void)::close(fd);
      fd = -1;
    }
  }
}

void Log::RequestReopen() noexcept {
  const int saved_errno = errno;
  if (started_.load(std::memory_order_relaxed)) {
    reopen_requested_.store(true, std::memory_order_release);
    if (reopen_pipe_[1] >= 0) {
      const char wake = 1;
      [[maybe_unused]] const auto written = ::write(reopen_pipe_[1], &wake, 1);
    }
  }
  errno = saved_errno;
}

void Log::ReopenLoop() {
  while (!stop_reopen_.load(std::memory_order_acquire)) {
    struct pollfd pfd{
        .fd = reopen_pipe_[0],
        .events = POLLIN,
        .revents = 0,
    };
    const int rc = ::poll(&pfd, 1, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    char drain[64];
    while (::read(reopen_pipe_[0], drain, sizeof(drain)) > 0) {
    }
    if (stop_reopen_.load(std::memory_order_acquire)) {
      break;
    }
    if (reopen_requested_.exchange(false, std::memory_order_acq_rel)) {
      if (ReopenLog()) {
        spdlog::info("Successfully reopened audit log");
      }
    }
  }
}

void Log::Record(const std::string &address, const std::string &username, const std::string &query,
                 const memgraph::communication::bolt::map_t &params, const std::string &db,
                 const std::string &impersonator) {
  if (!started_.load(std::memory_order_relaxed)) return;
  auto [redacted_query, redacted_params] = RedactAuditQueryAndParams(query, params);
  auto timestamp =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  buffer_->emplace(Item{.timestamp = timestamp,
                        .address = address,
                        .username = username,
                        .impersonator = impersonator,
                        .query = std::move(redacted_query),
                        .params = std::move(redacted_params),
                        .db = db});
}

bool Log::ReopenLogLocked() {
  utils::OutputFile replacement;
  auto const res = replacement.Open(storage_directory_ / "audit.log", utils::OutputFile::Mode::APPEND_TO_EXISTING);
  if (!res) {
    spdlog::warn("Failed to reopen audit log file. Continuing to write to the existing audit log handle.");
    return false;
  }
  if (log_.IsOpen()) {
    std::ignore = log_.Sync();
  }
  log_ = std::move(replacement);
  return true;
}

bool Log::ReopenLog() {
  if (!started_.load(std::memory_order_relaxed)) {
    spdlog::warn("Failed to reopen audit log file since audit log isn't started.");
    return false;
  }
  reopen_requested_.store(false, std::memory_order_release);
  auto guard = std::lock_guard{lock_};
  return ReopenLogLocked();
}

void Log::Flush() {
  auto guard = std::lock_guard{lock_};
  if (reopen_requested_.exchange(false, std::memory_order_acq_rel)) {
    if (ReopenLogLocked()) {
      spdlog::info("Successfully reopened audit log");
    }
  }
  for (uint64_t i = 0; i < buffer_size_; ++i) {
    auto item = buffer_->pop();
    if (!item) break;

    auto params_json = nlohmann::json::object();
    for (const auto &[k, v] : item->params) {
      params_json.push_back(nlohmann::json::object_t::value_type(k, BoltValueToJson(v)));
    }

    // log_ crashes on a failed write, so these never report an error.
    std::ignore = log_.Write(fmt::format("{}.{:06d},{},{},{},{},{},{}\n",
                                         item->timestamp / 1'000'000,
                                         item->timestamp % 1'000'000,
                                         utils::EncodeAuditCsvField(item->address),
                                         utils::EncodeAuditCsvField(item->username),
                                         utils::EncodeAuditCsvField(item->impersonator),
                                         utils::EncodeAuditCsvField(item->db),
                                         utils::EncodeAuditCsvField(item->query),
                                         utils::EncodeAuditCsvField(params_json.dump())));
  }
  std::ignore = log_.Sync();
}

}  // namespace memgraph::audit
