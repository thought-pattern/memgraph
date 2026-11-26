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

#include "storage/v2/ttl.hpp"
#include "memory/db_arena_fwd.hpp"

#include "storage/v2/access_type.hpp"
#include "storage/v2/inmemory/storage.hpp"

#ifdef MG_ENTERPRISE

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "metrics/prometheus_metrics.hpp"
#include "storage/v2/edge_accessor.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/storage.hpp"
#include "storage/v2/storage_mode.hpp"
#include "storage/v2/vertex_accessor.hpp"
#include "storage/v2/view.hpp"
#include "utils/bound.hpp"
#include "utils/temporal.hpp"

namespace {
template <typename T>
int GetPart(auto &current) {
  const int whole_part = std::chrono::duration_cast<T>(current).count();
  current -= T{whole_part};
  return whole_part;
}
}  // namespace

namespace memgraph::storage::ttl {

std::chrono::microseconds TtlInfo::ParsePeriod(std::string_view sv) {
  if (sv.empty()) {
    throw TtlException("Badly defined period. Use integers and 'd', 'h', 'm' and 's' to define it.");
  }
  std::chrono::microseconds total{0};
  std::string_view remaining = sv;

  while (!remaining.empty()) {
    size_t pos = 0;
    while (pos < remaining.size() && std::isdigit(static_cast<unsigned char>(remaining[pos]))) {
      ++pos;
    }
    if (pos == 0) {
      throw TtlException("Badly defined period. Use integers and 'd', 'h', 'm' and 's' to define it.");
    }

    int64_t value = 0;
    try {
      value = std::stoll(std::string(remaining.substr(0, pos)));
    } catch (const std::exception &) {
      throw TtlException("Badly defined period. Use integers and 'd', 'h', 'm' and 's' to define it.");
    }
    remaining = remaining.substr(pos);

    if (remaining.empty()) {
      throw TtlException("Badly defined period. Use integers and 'd', 'h', 'm' and 's' to define it.");
    }

    const auto unit = remaining[0];
    remaining = remaining.substr(1);

    switch (unit) {
      case 'd':
        total += std::chrono::days{value};
        break;
      case 'h':
        total += std::chrono::hours{value};
        break;
      case 'm':
        total += std::chrono::minutes{value};
        break;
      case 's':
        total += std::chrono::seconds{value};
        break;
      default:
        throw TtlException("Badly defined period. Use integers and 'd', 'h', 'm' and 's' to define it.");
    }
  }

  if (total <= std::chrono::microseconds{0}) {
    throw TtlException("TTL period must be greater than zero.");
  }

  return total;
}

std::string TtlInfo::StringifyPeriod(std::chrono::microseconds us) {
  auto remaining = us;
  std::string result;

  const auto days = GetPart<std::chrono::days>(remaining);
  if (days > 0) {
    result += fmt::format("{}d", days);
  }

  const auto hours = GetPart<std::chrono::hours>(remaining);
  if (hours > 0) {
    result += fmt::format("{}h", hours);
  }

  const auto minutes = GetPart<std::chrono::minutes>(remaining);
  if (minutes > 0) {
    result += fmt::format("{}m", minutes);
  }

  const auto seconds = GetPart<std::chrono::seconds>(remaining);
  if (seconds > 0) {
    result += fmt::format("{}s", seconds);
  }

  return result;
}

std::chrono::system_clock::time_point TtlInfo::ParseStartTime(std::string_view sv,
                                                              std::chrono::system_clock::time_point reference_now) {
  try {
    const auto now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(reference_now.time_since_epoch()).count();
    const auto local_today = utils::LocalDateTime{now_us}.date();
    const utils::DateParameters date{.year = static_cast<int64_t>(local_today.year),
                                     .month = static_cast<int64_t>(local_today.month),
                                     .day = static_cast<int64_t>(local_today.day)};
    auto [time, _] = utils::ParseLocalTimeParameters(sv);
    return std::chrono::system_clock::time_point{
        std::chrono::microseconds{utils::LocalDateTime(date, time).SysMicrosecondsSinceEpoch()}};
  } catch (const utils::temporal::InvalidArgumentException &e) {
    throw TtlException(e.what());
  }
}

std::chrono::system_clock::time_point TtlInfo::ParseStartTime(std::string_view sv) {
  return ParseStartTime(sv, std::chrono::system_clock::now());
}

std::string TtlInfo::StringifyStartTime(std::chrono::system_clock::time_point st) {
  const utils::LocalDateTime ldt(std::chrono::duration_cast<std::chrono::microseconds>(st.time_since_epoch()).count());
  auto epoch = std::chrono::microseconds{ldt.MicrosecondsSinceEpoch()};
  /* just consume and through away */
  GetPart<std::chrono::days>(epoch);
  /* what we are actually interested in */
  const auto h = GetPart<std::chrono::hours>(epoch);
  const auto m = GetPart<std::chrono::minutes>(epoch);
  const auto s = GetPart<std::chrono::seconds>(epoch);
  return fmt::format("{:02d}:{:02d}:{:02d}", h, m, s);
}

std::chrono::microseconds TTL::ValidateInterval(std::optional<std::chrono::microseconds> period) {
  auto actual_period = period.value_or(std::chrono::days(1));
  if (actual_period <= std::chrono::microseconds{0}) {
    throw TtlException("TTL period must be greater than zero.");
  }
  if (std::chrono::duration_cast<std::chrono::milliseconds>(actual_period) <= std::chrono::milliseconds{0}) {
    actual_period = std::chrono::milliseconds{1};
  }
  return actual_period;
}

void TTL::SetInterval(std::optional<std::chrono::microseconds> period,
                      std::optional<std::chrono::system_clock::time_point> start_time) {
  if (!enabled_.load(std::memory_order_acquire)) {
    throw TtlException("TTL not enabled!");
  }
  if (!ttl_.IsRunning()) {
    throw TtlException("TTL not running!");
  }

  auto actual_period = ValidateInterval(period);

  // Update the info_ with the new period and start_time
  info_.period = actual_period;
  info_.start_time = start_time;

  // TTL's schedule is set after the worker thread is already running (Configure() starts it first),
  // so without waking it here the worker could stay parked and TTL would silently never fire.
  ttl_.SetIntervalAndWake(actual_period, start_time);
}

void TTL::Configure(bool should_run_edge_ttl) {
  if (!enabled_.load(std::memory_order_acquire)) {
    throw TtlException("TTL not enabled!");
  }
  if (ttl_.IsRunning()) {
    throw TtlException("TTL already running!");
  }

  info_.should_run_edge_ttl = should_run_edge_ttl;

  if (info_.should_run_edge_ttl && (storage_ptr_->GetStorageMode() == StorageMode::ON_DISK_TRANSACTIONAL ||
                                    !storage_ptr_->config_.salient.items.properties_on_edges)) {
    spdlog::warn("Memgraph configuration doesn't support edge TTL. Edge TTL will be disabled.");
    info_.should_run_edge_ttl = false;
  }

  auto ttl_job = [this]() {
    const memory::DbArenaScope db_arena_scope{storage_ptr_->DbArenaPool()};
    auto const should_abort_run = [this]() {
      return !enabled_.load(std::memory_order_acquire) || !wanted_.load(std::memory_order_acquire) || Paused() ||
             !user_check_();
    };
    if (should_abort_run()) return;

    bool finished_vertex = false;
    bool finished_edge = !info_.should_run_edge_ttl;
    const auto now = std::chrono::system_clock::now();
    const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch());

    spdlog::trace("Running TTL at {}",
                  std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());

    const auto ttl_label = storage_ptr_->NameToLabel("TTL");
    const auto ttl_property = storage_ptr_->NameToProperty("ttl");

    while (!finished_vertex || !finished_edge) {
      if (should_abort_run()) return;
      try {
        constexpr size_t batch_size = 10'000;
        size_t n_deleted = 0;
        size_t n_edges_deleted = 0;
        bool batch_finishes_vertex = false;
        bool batch_finishes_edge = false;

        // Create a new transaction for this batch
        // This ensures each batch is isolated and can be retried independently
        auto batch_accessor = storage_ptr_->Access(storage::StorageAccessType::WRITE);
        if (should_abort_run()) return;

        // Verify required indices exist and are ready for TTL operations
        // This ensures TTL can work efficiently using range-based filtering

        std::vector<PropertyPath> ttl_property_path = {ttl_property};
        bool const missing_lp_index = !batch_accessor->LabelPropertyIndexExists(ttl_label, ttl_property_path);
        bool const missing_edge_index = !batch_accessor->EdgePropertyIndexExists(ttl_property);

        if (missing_lp_index) {
          spdlog::warn(
              "TTL requires label+property index on :TTL(ttl) but it doesn't exist. Will create it automatically.");
          std::vector<PropertyPath> create_path = {ttl_property};

          if (storage_ptr_->GetStorageMode() == StorageMode::IN_MEMORY_TRANSACTIONAL) {
            auto *mem_storage = static_cast<storage::InMemoryStorage *>(storage_ptr_);
            mem_storage->GetAsyncIndexer().Enqueue(ttl_label, std::move(create_path));
          } else {
            batch_accessor.reset();
            auto ddl_accessor = storage_ptr_->UniqueAccess();
            if (should_abort_run()) return;
            auto ddl_protector = storage_ptr_->make_database_protector();
            if (!ddl_protector) return;
            if (ddl_accessor->CreateIndex(ttl_label, std::move(create_path)).has_value()) {
              (void)ddl_accessor->PrepareForCommitPhase(CommitArgs::make_main(std::move(ddl_protector)));
            }
            ddl_accessor.reset();
            batch_accessor = storage_ptr_->Access(storage::StorageAccessType::WRITE);
            if (should_abort_run()) return;
          }
        }

        if (info_.should_run_edge_ttl && missing_edge_index) {
          spdlog::warn(
              "TTL requires edge property index on ttl property but it doesn't exist. Will create it automatically.");
          if (storage_ptr_->GetStorageMode() == StorageMode::IN_MEMORY_TRANSACTIONAL) {
            auto *mem_storage = static_cast<storage::InMemoryStorage *>(storage_ptr_);
            mem_storage->GetAsyncIndexer().Enqueue(ttl_property);
          } else {
            batch_accessor.reset();
            auto ddl_accessor = storage_ptr_->UniqueAccess();
            if (should_abort_run()) return;
            auto ddl_protector = storage_ptr_->make_database_protector();
            if (!ddl_protector) return;
            if (ddl_accessor->CreateGlobalEdgeIndex(ttl_property).has_value()) {
              (void)ddl_accessor->PrepareForCommitPhase(CommitArgs::make_main(std::move(ddl_protector)));
            }
            ddl_accessor.reset();
            batch_accessor = storage_ptr_->Access(storage::StorageAccessType::WRITE);
            if (should_abort_run()) return;
          }
        }

        bool const lp_index_ready = batch_accessor->LabelPropertyIndexReady(ttl_label, ttl_property_path);
        bool const edge_index_ready = info_.should_run_edge_ttl && batch_accessor->EdgePropertyIndexReady(ttl_property);

        // Process vertices with TTL label and ttl property using label+property index with range
        if (!finished_vertex && lp_index_ready) {
          std::vector<PropertyValueRange> ttl_property_ranges = {
              PropertyValueRange::Bounded(std::nullopt, utils::MakeBoundExclusive(PropertyValue(now_us.count())))};
          auto vertices = batch_accessor->Vertices(ttl_label, ttl_property_path, ttl_property_ranges, View::NEW);
          std::vector<VertexAccessor> vertices_to_delete;
          vertices_to_delete.reserve(batch_size);

          for (const auto &vertex : vertices) {
            vertices_to_delete.push_back(vertex);
            if (vertices_to_delete.size() >= batch_size) break;  // Batch size limit
          }

          if (!vertices_to_delete.empty()) {
            std::vector<VertexAccessor *> vertices_to_delete_pointers;
            vertices_to_delete_pointers.reserve(vertices_to_delete.size());
            for (auto &vertex : vertices_to_delete) {
              vertices_to_delete_pointers.push_back(&vertex);
            }
            auto result = batch_accessor->DetachDelete(vertices_to_delete_pointers, {}, true);
            if (!result.has_value()) {
              batch_accessor->Abort();
              std::this_thread::yield();
              continue;
            }
            if (result->has_value()) {
              n_deleted += (*result)->first.size();
              n_edges_deleted += (*result)->second.size();
            }
          }

          batch_finishes_vertex = vertices_to_delete.size() < batch_size;
        } else if (!finished_edge && edge_index_ready) {
          auto edges = batch_accessor->Edges(
              ttl_property,
              PropertyValueRange::Bounded(std::nullopt, utils::MakeBoundExclusive(PropertyValue(now_us.count()))),
              View::NEW);
          std::vector<EdgeAccessor> edges_to_delete;
          edges_to_delete.reserve(batch_size);

          for (const auto &edge : edges) {
            edges_to_delete.push_back(edge);
            if (edges_to_delete.size() >= batch_size) break;  // Batch size limit
          }

          if (!edges_to_delete.empty()) {
            std::vector<EdgeAccessor *> edges_to_delete_pointers;
            edges_to_delete_pointers.reserve(edges_to_delete.size());
            for (auto &edge : edges_to_delete) {
              edges_to_delete_pointers.push_back(&edge);
            }
            auto result = batch_accessor->DetachDelete({}, edges_to_delete_pointers, false);
            if (!result.has_value()) {
              batch_accessor->Abort();
              std::this_thread::yield();
              continue;
            }
            if (result->has_value()) {
              n_edges_deleted += (*result)->second.size();
            }
          }

          batch_finishes_edge = edges_to_delete.size() < batch_size;
        } else if (!lp_index_ready || (info_.should_run_edge_ttl && !edge_index_ready)) {
          spdlog::info("TTL indices not ready, skipping this run.");
          return;
        } else {
          DMG_ASSERT(false, "Unsupported TTL state.");
        }

        if (should_abort_run()) {
          batch_accessor->Abort();
          return;
        }

        auto protector = storage_ptr_->make_database_protector();
        if (!protector) [[unlikely]] {
          batch_accessor->Abort();
          return;
        }

        // Commit the transaction for this batch using the database protector
        auto commit_result = batch_accessor->PrepareForCommitPhase(CommitArgs::make_main(std::move(protector)));
        bool committed_on_main = commit_result.has_value();
        if (!committed_on_main) {
          if (auto const *repl_err = std::get_if<ReplicationError>(&commit_result.error());
              repl_err != nullptr && repl_err->transaction_committed) {
            committed_on_main = true;
          }
        }
        if (!committed_on_main) {
          continue;
        }

        if (batch_finishes_vertex) finished_vertex = true;
        if (batch_finishes_edge) finished_edge = true;
        deleted_nodes_.Increment(static_cast<double>(n_deleted));
        deleted_edges_.Increment(static_cast<double>(n_edges_deleted));

      } catch (const std::exception &e) {
        spdlog::trace("TTL error; retrying later: {}", e.what());
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
      }

      std::this_thread::yield();
    }

    spdlog::trace("Finished TTL run from {}",
                  std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
  };

  // Starts the TTL job, but will not run until the period is set
  ttl_.Pause();
  ttl_.Run("storage-ttl", std::move(ttl_job));
}

}  // namespace memgraph::storage::ttl

#endif  // MG_ENTERPRISE
