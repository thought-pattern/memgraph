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

#include "coordination/coordinator_log_store.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <charconv>
#include <cstddef>
#include <exception>
#include <libnuraft/buffer.hxx>
#include <libnuraft/buffer_serializer.hxx>
#include <libnuraft/log_entry.hxx>
#include <libnuraft/log_val_type.hxx>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <string>

#include "coordination/constants.hpp"
#include "coordination/coordinator_communication_config.hpp"
#include "coordination/coordinator_state_manager.hpp"
#include "utils/logging.hpp"

namespace memgraph::coordination {
using nuraft::buffer_serializer;

namespace {
std::shared_ptr<log_entry> MakeClone(const std::shared_ptr<log_entry> &entry) {
  return std::make_shared<log_entry>(
      entry->get_term(), buffer::clone(entry->get_buf()), entry->get_val_type(), entry->get_timestamp());
}

std::optional<uint64_t> ParseLogEntryIndexFromKey(std::string_view key) {
  if (!key.starts_with(kLogEntryPrefix)) return std::nullopt;
  auto const suffix = key.substr(kLogEntryPrefix.size());
  if (suffix.empty()) return std::nullopt;
  uint64_t idx = 0;
  auto const [ptr, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), idx);
  if (ec != std::errc{} || ptr != suffix.data() + suffix.size()) return std::nullopt;
  return idx;
}
}  // namespace

CoordinatorLogStore::CoordinatorLogStore(LoggerWrapper const logger, LogStoreDurability log_store_durability)
    : durability_(std::move(log_store_durability.durability_store_)), logger_(logger) {
  logger_.Log(nuraft_log_level::INFO, "Restoring coordinator log store with durability.");
  MG_ASSERT(HandleVersionMigration(log_store_durability.stored_log_store_version_),
            "Couldn't handle version migration in coordinator log store.");
}

bool CoordinatorLogStore::HandleVersionMigration(LogStoreVersion const stored_version) {
  if constexpr (kActiveVersion == LogStoreVersion::kV2) {
    if (stored_version == LogStoreVersion::kV1 || stored_version == LogStoreVersion::kV2) {
      auto const maybe_last_log_entry = durability_->GetStrict(kLastLogEntry);
      auto const maybe_start_idx = durability_->GetStrict(kStartIdx);

      if (!maybe_last_log_entry && maybe_last_log_entry.error() == kvstore::KVStore::GetError::kReadError) {
        logger_.Log(nuraft_log_level::ERROR, "Failed to read last_log_entry from coordinator log store durability.");
        return false;
      }
      if (!maybe_start_idx && maybe_start_idx.error() == kvstore::KVStore::GetError::kReadError) {
        logger_.Log(nuraft_log_level::ERROR, "Failed to read start_idx from coordinator log store durability.");
        return false;
      }
      if (maybe_last_log_entry.has_value() != maybe_start_idx.has_value()) {
        logger_.Log(nuraft_log_level::ERROR,
                    "Inconsistent coordinator log store metadata: only one of start_idx and last_log_entry exists.");
        return false;
      }

      if (!maybe_last_log_entry && !maybe_start_idx) {
        auto it = durability_->begin(std::string{kLogEntryPrefix});
        auto const end = durability_->end(std::string{kLogEntryPrefix});
        if (it != end || it.HasError()) {
          logger_.Log(nuraft_log_level::ERROR,
                      "Refusing to initialize empty Raft log boundaries while existing log entries or scan errors are "
                      "present on disk.");
          return false;
        }
        logger_.Log(nuraft_log_level::INFO,
                    "Confirmed empty coordinator log store on disk, initializing first-start log boundaries.");
        std::map<std::string, std::string> batch;
        batch.emplace(kStartIdx, std::to_string(kInitialStartIdx));
        batch.emplace(kLastLogEntry, std::to_string(kInitialLastLogEntry));
        if (!durability_->PutMultiple(batch)) {
          logger_.Log(nuraft_log_level::ERROR, "Failed to persist initial coordinator log store boundaries.");
          return false;
        }
        start_idx_.store(kInitialStartIdx, std::memory_order_release);
        return true;
      }

      uint64_t last_log_entry = 0;
      uint64_t durable_start_idx_value = 0;
      try {
        last_log_entry = std::stoull(maybe_last_log_entry.value());
        durable_start_idx_value = std::stoull(maybe_start_idx.value());
      } catch (std::exception const &e) {
        logger_.Log(nuraft_log_level::ERROR, fmt::format("Corrupted Raft log boundary metadata: {}", e.what()));
        return false;
      }
      if (durable_start_idx_value == 0) {
        logger_.Log(nuraft_log_level::ERROR, "Invalid start_idx 0 in coordinator log store durability.");
        return false;
      }
      start_idx_.store(durable_start_idx_value, std::memory_order_release);

      if (durable_start_idx_value > last_log_entry + 1) {
        auto const repaired_last_log_entry = durable_start_idx_value - 1;
        logger_.Log(nuraft_log_level::WARNING,
                    fmt::format("Start index {} is beyond last log entry {}, treating the log store as empty and "
                                "repairing last log entry to {}.",
                                durable_start_idx_value,
                                last_log_entry,
                                repaired_last_log_entry));
        return durability_->Put(kLastLogEntry, std::to_string(repaired_last_log_entry));
      }

      for (auto const id : std::ranges::iota_view{durable_start_idx_value, last_log_entry + 1}) {
        auto const entry = durability_->GetStrict(fmt::format("{}{}", kLogEntryPrefix, id));
        if (!entry) {
          logger_.Log(nuraft_log_level::ERROR,
                      fmt::format("Discontinuous or unreadable Raft log entry {} in retained range [{}:{}].",
                                  id,
                                  durable_start_idx_value,
                                  last_log_entry));
          return false;
        }

        try {
          auto const j = nlohmann::json::parse(entry.value());
          auto const term = j.at(kLogEntryTermKey).get<int>();
          auto const data = j.at(kLogEntryDataKey).get<std::string>();
          auto const value_type = j.at("val_type").get<int>();
          auto const val_type_enum = static_cast<nuraft::log_val_type>(value_type);

          std::shared_ptr<buffer> log_term_buffer;
          if (val_type_enum == nuraft::log_val_type::conf) {
            if (data.empty()) {
              LOG_FATAL(
                  "Detected old-format coordinator config log entry at index {} with empty data. "
                  "This version requires a clean coordinator data directory. "
                  "Please stop all coordinators, remove their data directories, and re-establish the HA cluster.",
                  id);
            }
            auto const config_json = nlohmann::json::parse(data);
            std::shared_ptr<nuraft::cluster_config> config;
            from_json(config_json, config);
            log_term_buffer = config->serialize();
          } else if (val_type_enum == nuraft::log_val_type::app_log) {
            log_term_buffer = buffer::alloc(sizeof(uint32_t) + data.size());
            buffer_serializer bs{log_term_buffer};
            bs.put_str(data);
          } else {
            LOG_FATAL("Invalid type of logs found in log store durability");
          }

          logs_[id] = std::make_shared<log_entry>(term, log_term_buffer, val_type_enum);
          logger_.Log(nuraft_log_level::TRACE,
                      fmt::format("Loaded entry from disk: ID {}, \n ENTRY {} ,\n DATA:{}, ", j.dump(), id, data));
        } catch (std::exception const &e) {
          LOG_FATAL("Error occurred while parsing JSON {}", e.what());
        }
      }
      return true;
    }
  }

  return false;
}

CoordinatorLogStore::~CoordinatorLogStore() = default;

auto CoordinatorLogStore::FindOrDefault_(ulong index) const -> std::shared_ptr<log_entry> {
  auto entry = logs_.find(index);
  if (entry == logs_.end()) {
    logger_.Log(nuraft_log_level::TRACE, fmt::format("Couldn't find log with index {} in the log storage.", index));
    return std::make_shared<log_entry>(0, buffer::alloc(sizeof(uint64_t)));
  }
  logger_.Log(nuraft_log_level::TRACE, fmt::format("Found log with index {} in the log storage.", index));
  return entry->second;
}

void CoordinatorLogStore::DeleteLogs(uint64_t start, uint64_t end) {
  std::vector<std::string> del_batch;
  auto it = logs_.lower_bound(start);
  while (it != logs_.end() && it->first <= end) {
    del_batch.push_back(fmt::format("{}{}", kLogEntryPrefix, it->first));
    ++it;
  }
  if (del_batch.empty()) return;
  if (!durability_->PutAndDeleteMultiple({}, del_batch)) {
    throw kvstore::KVStoreError("Failed to delete Raft log range from RocksDB");
  }
  logs_.erase(logs_.lower_bound(start), logs_.upper_bound(end));
}

ulong CoordinatorLogStore::next_slot() const {
  auto lock = std::lock_guard{logs_lock_};
  return GetNextSlot();
}

auto CoordinatorLogStore::GetNextSlot() const -> ulong {
  auto const s_idx = start_idx_.load(std::memory_order_acquire);
  if (logs_.empty()) {
    return s_idx;
  }
  return std::max<ulong>(s_idx, logs_.rbegin()->first + 1);
}

ulong CoordinatorLogStore::start_index() const { return start_idx_.load(std::memory_order_acquire); }

std::shared_ptr<log_entry> CoordinatorLogStore::last_entry() const {
  auto lock = std::lock_guard{logs_lock_};
  ulong const next_slot = GetNextSlot();
  auto const last_src = FindOrDefault_(next_slot - 1);

  return MakeClone(last_src);
}

ulong CoordinatorLogStore::append(std::shared_ptr<log_entry> &entry) {
  auto const clone = MakeClone(entry);
  auto lock = std::lock_guard{logs_lock_};
  ulong const next_slot = GetNextSlot();

  bool constexpr is_entry_with_biggest_id{true};
  if (!StoreEntryToDisk(clone, next_slot, is_entry_with_biggest_id)) {
    throw kvstore::KVStoreError(fmt::format("Failed to persist appended Raft log entry at index {}", next_slot));
  }

  spdlog::trace("Appended log at index {} to the log storage.", next_slot);
  logs_[next_slot] = clone;

  return next_slot;
}

void CoordinatorLogStore::write_at(ulong index, std::shared_ptr<log_entry> &entry) {
  std::shared_ptr<log_entry> const clone = MakeClone(entry);

  auto lock = std::lock_guard{logs_lock_};
  std::set<std::string> displaced_keys;
  for (auto itr = logs_.upper_bound(index); itr != logs_.end(); ++itr) {
    displaced_keys.insert(fmt::format("{}{}", kLogEntryPrefix, itr->first));
  }
  auto db_it = durability_->begin(std::string{kLogEntryPrefix});
  auto const db_end = durability_->end(std::string{kLogEntryPrefix});
  for (; db_it != db_end; ++db_it) {
    if (auto const parsed = ParseLogEntryIndexFromKey(db_it->first); parsed.has_value() && *parsed > index) {
      displaced_keys.insert(db_it->first);
    }
  }
  if (db_it.HasError()) {
    throw kvstore::KVStoreError(
        fmt::format("Failed to scan displaced Raft log suffix above index {}: {}", index, db_it.ErrorMessage()));
  }

  std::vector<std::string> const del_batch(displaced_keys.begin(), displaced_keys.end());
  // Replacing at `index` truncates every entry above `index`, making `index` the newest log entry.
  bool const is_entry_with_biggest_id = index != 0;
  if (!StoreEntryToDisk(clone, index, is_entry_with_biggest_id, del_batch)) {
    throw kvstore::KVStoreError(fmt::format("Failed to persist Raft log replacement at index {}", index));
  }

  logs_.erase(logs_.lower_bound(index), logs_.end());
  if (index != 0) [[likely]] {
    logs_[index] = clone;
  }
}

std::shared_ptr<std::vector<std::shared_ptr<log_entry>>> CoordinatorLogStore::log_entries(ulong start, ulong end) {
  auto ret = std::make_shared<std::vector<std::shared_ptr<log_entry>>>();
  if (end <= start) return ret;
  ret->reserve(end - start);

  for (ulong i = start; i < end; i++) {
    std::shared_ptr<log_entry> src;
    {
      auto lock = std::lock_guard{logs_lock_};
      auto const entry = logs_.find(i);
      if (entry == logs_.end()) {
        spdlog::trace("Could not find entry at index {}", i);
        return nullptr;
      }
      src = entry->second;
    }
    ret->emplace_back(MakeClone(src));
  }
  return ret;
}

std::vector<std::pair<int64_t, std::shared_ptr<log_entry>>> CoordinatorLogStore::GetAllEntriesRange(
    uint64_t start, uint64_t end) const {
  auto lock = std::lock_guard{logs_lock_};
  auto const effective_start = std::max<uint64_t>(start, start_idx_.load(std::memory_order_acquire));
  std::vector<std::pair<int64_t, std::shared_ptr<log_entry>>> entries;
  if (end <= effective_start) {
    return entries;
  }
  entries.reserve(end - effective_start);

  for (uint64_t i = effective_start; i < end; i++) {
    auto const entry = logs_.find(i);
    if (entry == logs_.end()) {
      throw kvstore::KVStoreError(fmt::format("Missing Raft log entry at index {} in range [{}, {})", i, effective_start, end));
    }
    entries.emplace_back(static_cast<int64_t>(i), MakeClone(entry->second));
  }
  return entries;
}

std::shared_ptr<log_entry> CoordinatorLogStore::entry_at(ulong index) {
  auto lock = std::lock_guard{logs_lock_};
  std::shared_ptr<log_entry> const src = FindOrDefault_(index);
  return MakeClone(src);
}

ulong CoordinatorLogStore::term_at(ulong index) {
  auto lock = std::lock_guard{logs_lock_};
  return FindOrDefault_(index)->get_term();
}

std::shared_ptr<buffer> CoordinatorLogStore::pack(ulong index, int32 cnt) {
  std::vector<std::shared_ptr<buffer>> logs;

  size_t size_total = 0;
  ulong const end_index = index + cnt;
  logger_.Log(nuraft_log_level::TRACE, fmt::format("Packing logs from {} to {}", index, end_index));
  for (ulong i = index; i < end_index; ++i) {
    std::shared_ptr<log_entry> le = nullptr;
    {
      auto lock = std::lock_guard{logs_lock_};
      if (auto elem = logs_.find(i); elem != logs_.end()) {
        le = elem->second;
      } else if (index == 0) {
        le = FindOrDefault_(0);
      }
    }
    MG_ASSERT(le != nullptr, "Could not find log entry at index {}", i);
    auto buf = le->serialize();
    size_total += buf->size();
    logs.push_back(buf);
  }

  auto buf_out = buffer::alloc(sizeof(int32) + cnt * sizeof(int32) + size_total);
  buf_out->pos(0);
  buf_out->put((int32)cnt);

  for (auto &entry : logs) {
    buf_out->put(static_cast<int32>(entry->size()));
    buf_out->put(*entry);
  }
  return buf_out;
}

void CoordinatorLogStore::apply_pack(ulong index, buffer &pack) {
  pack.pos(0);
  int32 const num_logs = pack.get_int();
  logger_.Log(nuraft_log_level::TRACE, fmt::format("Applying pack for logs from {} to {}", index, index + num_logs));
  for (int32 i = 0; i < num_logs; ++i) {
    auto const cur_idx = index + i;
    auto const buf_size = pack.get_int();

    std::shared_ptr<buffer> buf_local = buffer::alloc(buf_size);
    pack.get(buf_local);

    std::shared_ptr<log_entry> const le = log_entry::deserialize(*buf_local);
    {
      auto lock = std::lock_guard{logs_lock_};
      bool const is_entry_with_biggest_id = cur_idx >= GetNextSlot();
      if (!StoreEntryToDisk(le, cur_idx, is_entry_with_biggest_id)) {
        throw kvstore::KVStoreError(fmt::format("Failed to persist unpacked Raft log entry at index {}", cur_idx));
      }
      if (cur_idx != 0) [[likely]] {
        logs_[cur_idx] = le;
      }
    }
  }
  {
    auto lock = std::lock_guard{logs_lock_};
    if (auto const entry = logs_.upper_bound(0); entry != logs_.end()) {
      if (!durability_->Put(kStartIdx, std::to_string(entry->first))) {
        throw kvstore::KVStoreError("Failed to persist updated Raft log start_idx during apply_pack");
      }
      start_idx_.store(entry->first, std::memory_order_release);
    } else {
      start_idx_.store(1, std::memory_order_release);
    }
  }
}

// NOTE: Remove all logs up to given 'last_log_index' (inclusive).
bool CoordinatorLogStore::compact(ulong last_log_index) {
  logger_.Log(nuraft_log_level::TRACE, fmt::format("Compacting logs up to {}", last_log_index));
  auto lock = std::lock_guard{logs_lock_};
  auto const old_start_idx = start_idx_.load(std::memory_order_acquire);
  auto const next_slot_before = GetNextSlot();
  bool const compacts_past_last_entry = next_slot_before == 0 || last_log_index >= next_slot_before - 1;

  std::set<std::string> keys_to_delete;
  for (auto it = logs_.begin(); it != logs_.end() && it->first <= last_log_index; ++it) {
    keys_to_delete.insert(fmt::format("{}{}", kLogEntryPrefix, it->first));
  }
  auto db_it = durability_->begin(std::string{kLogEntryPrefix});
  auto const db_end = durability_->end(std::string{kLogEntryPrefix});
  for (; db_it != db_end; ++db_it) {
    if (auto const parsed = ParseLogEntryIndexFromKey(db_it->first);
        parsed.has_value() && *parsed <= last_log_index) {
      keys_to_delete.insert(db_it->first);
    }
  }
  if (db_it.HasError()) {
    logger_.Log(nuraft_log_level::ERROR,
                fmt::format("Failed to scan Raft log keys during compaction up to {}: {}",
                            last_log_index,
                            db_it.ErrorMessage()));
    return false;
  }

  std::vector<std::string> const del_batch(keys_to_delete.begin(), keys_to_delete.end());
  std::map<std::string, std::string> put_batch;
  if (old_start_idx <= last_log_index) {
    auto const new_idx = last_log_index + 1;
    put_batch.emplace(kStartIdx, std::to_string(new_idx));
    if (compacts_past_last_entry) {
      put_batch.emplace(kLastLogEntry, std::to_string(last_log_index));
    }
  }

  if (!put_batch.empty() || !del_batch.empty()) {
    if (!durability_->PutAndDeleteMultiple(put_batch, del_batch)) {
      logger_.Log(nuraft_log_level::ERROR,
                  fmt::format("Failed to persist Raft log compaction batch up to {}", last_log_index));
      return false;
    }
  }

  logs_.erase(logs_.begin(), logs_.upper_bound(last_log_index));
  if (old_start_idx <= last_log_index) {
    start_idx_.store(last_log_index + 1, std::memory_order_release);
  }
  return true;
}

bool CoordinatorLogStore::flush() {
  spdlog::trace("Synced WAL to make Raft logs durable.");
  return durability_->SyncWal();
}

bool CoordinatorLogStore::StoreEntryToDisk(const std::shared_ptr<log_entry> &clone, uint64_t key_id,
                                           bool is_newest_entry, const std::vector<std::string> &keys_to_delete) {
  auto const data_string = [&clone, logger = &logger_]() -> std::string {
    auto const log_type = clone->get_val_type();
    if (log_type == nuraft::log_val_type::conf) {
      logger->Log(nuraft_log_level::TRACE, "Received config log, serializing cluster config as JSON.");
      auto config = nuraft::cluster_config::deserialize(clone->get_buf());
      nlohmann::json j;
      to_json(j, *config);
      return j.dump();
    }
    if (log_type == nuraft::log_val_type::app_log) {
      logger->Log(nuraft_log_level::TRACE, "Received application log, serializing it.");
      buffer_serializer bs(clone->get_buf());
      return bs.get_str();
    }
    LOG_FATAL("Unknown log type when trying to store entry to disk {}", static_cast<int>(log_type));
  }();

  auto const clone_val = static_cast<int>(clone->get_val_type());
  logger_.Log(nuraft_log_level::TRACE,
              fmt::format("Storing entry to disk: ID {}, \n ENTRY {} \n type {}", key_id, data_string, clone_val));

  auto const log_term_json = nlohmann::json(
      {{kLogEntryTermKey, clone->get_term()}, {kLogEntryDataKey, data_string}, {kLogEntryValTypeKey, clone_val}});
  auto const log_term_str = log_term_json.dump();
  auto const log_entry_key = fmt::format("{}{}", kLogEntryPrefix, key_id);

  std::map<std::string, std::string> put_batch;
  if (key_id != 0) {
    put_batch.emplace(log_entry_key, log_term_str);
  }

  if (is_newest_entry) {
    logger_.Log(nuraft_log_level::TRACE, fmt::format("Storing newest entry to disk {}", key_id));
    put_batch.emplace(kLastLogEntry, std::to_string(key_id));
  }

  if (!keys_to_delete.empty()) {
    return durability_->PutAndDeleteMultiple(put_batch, keys_to_delete);
  }
  return durability_->PutMultiple(put_batch);
}
}  // namespace memgraph::coordination
#endif
