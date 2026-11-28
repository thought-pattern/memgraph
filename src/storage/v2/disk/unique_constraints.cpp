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

#include "storage/v2/disk/unique_constraints.hpp"
#include <charconv>
#include <rocksdb/utilities/transaction.h>
#include <limits>
#include <optional>
#include <tuple>
#include "kvstore/kvstore.hpp"
#include "metrics/prometheus_metrics.hpp"
#include "spdlog/spdlog.h"
#include "storage/v2/constraints/unique_constraints.hpp"
#include "storage/v2/disk/delta_utils.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/property_value_utils.hpp"
#include "storage/v2/storage.hpp"
#include "storage/v2/vertex.hpp"
#include "utils/exceptions.hpp"
#include "utils/file.hpp"

namespace memgraph::storage {

namespace {

bool IsVertexUnderConstraint(const Vertex &vertex, const LabelId &constraint_label,
                             const std::set<PropertyId> &constraint_properties) {
  return std::ranges::contains(vertex.labels, constraint_label) &&
         vertex.properties.HasAllProperties(constraint_properties);
}

[[nodiscard]] bool ClearTransactionEntriesWithRemovedConstraintLabel(
    rocksdb::Transaction &disk_transaction,
    const std::map<Gid, std::set<std::pair<LabelId, std::set<PropertyId>>>> &transaction_entries) {
  for (const auto &[vertex_gid, constraints] : transaction_entries) {
    for (const auto &[constraint_label, constraint_properties] : constraints) {
      auto key_to_delete = utils::SerializeVertexAsKeyForUniqueConstraint(
          constraint_label, constraint_properties, vertex_gid.ToString());
      if (auto status = disk_transaction.Delete(key_to_delete); !status.ok()) {
        return false;
      }
    }
  }
  return true;
}

bool CommitWithTimestamp(rocksdb::Transaction *disk_transaction, uint64_t commit_ts) {
  if (!disk_transaction->SetCommitTimestamp(commit_ts).ok()) {
    return false;
  }
  const auto status = disk_transaction->Commit();
  if (!status.ok()) {
    spdlog::error("rocksdb: {}", status.ToString());
  }
  return status.ok();
}

}  // namespace

// --- ActiveConstraints implementation ---

std::vector<std::pair<LabelId, std::set<PropertyId>>> DiskUniqueConstraints::ActiveConstraints::ListConstraints(
    [[maybe_unused]] uint64_t start_timestamp) const {
  return {constraints_->constraints_.begin(), constraints_->constraints_.end()};
}

void DiskUniqueConstraints::ActiveConstraints::UpdateBeforeCommit([[maybe_unused]] const Vertex *vertex,
                                                                  [[maybe_unused]] const Transaction &tx) {}

auto DiskUniqueConstraints::ActiveConstraints::GetAbortProcessor() const -> AbortProcessor { return AbortProcessor{}; }

void DiskUniqueConstraints::ActiveConstraints::CollectForAbort([[maybe_unused]] AbortProcessor &processor,
                                                               [[maybe_unused]] Vertex const *vertex) const {}

void DiskUniqueConstraints::ActiveConstraints::AbortEntries([[maybe_unused]] AbortableInfo &&info,
                                                            uint64_t exact_start_timestamp) {
  ClearEntriesForTransaction(exact_start_timestamp);
}

bool DiskUniqueConstraints::ActiveConstraints::empty() const { return constraints_->constraints_.empty(); }

DiskUniqueConstraints::TxConstraintDeletionMap DiskUniqueConstraints::ActiveConstraints::ExtractEntriesForTransaction(
    uint64_t transaction_start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  auto it = entries_for_deletion.find(transaction_start_timestamp);
  if (it == entries_for_deletion.end()) {
    return {};
  }
  auto extracted = std::move(it->second);
  entries_for_deletion.erase(it);
  return extracted;
}

void DiskUniqueConstraints::ActiveConstraints::ClearEntriesForTransaction(uint64_t transaction_start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  entries_for_deletion.erase(transaction_start_timestamp);
}

auto DiskUniqueConstraints::GetActiveConstraints() const -> std::shared_ptr<UniqueConstraints::ActiveConstraints> {
  return std::make_shared<ActiveConstraints>(this);
}

DiskUniqueConstraints::DiskUniqueConstraints(const Config &config, metrics::GaugeHandle gauge) : gauge_{gauge} {
  kvstore_ = std::make_unique<RocksDBStorage>();
  utils::EnsureDirOrDie(config.disk.unique_constraints_directory);
  kvstore_->options_.create_if_missing = true;
  kvstore_->InstallComparator(std::make_unique<ComparatorWithU64TsImpl>());
  logging::AssertRocksDBStatus(rocksdb::TransactionDB::Open(
      kvstore_->options_, rocksdb::TransactionDBOptions(), config.disk.unique_constraints_directory, &kvstore_->db_));
}

std::unique_ptr<rocksdb::Transaction> DiskUniqueConstraints::CreateAllReadingRocksDBTransaction() const {
  auto tx = std::unique_ptr<rocksdb::Transaction>(
      kvstore_->db_->BeginTransaction(rocksdb::WriteOptions(), rocksdb::TransactionOptions()));
  tx->SetReadTimestampForValidation(std::numeric_limits<uint64_t>::max());
  return tx;
}

bool DiskUniqueConstraints::CommitTransaction(rocksdb::Transaction *disk_transaction, uint64_t commit_timestamp) {
  return CommitWithTimestamp(disk_transaction, commit_timestamp);
}

bool DiskUniqueConstraints::InsertConstraint(
    LabelId label, const std::set<PropertyId> &properties,
    const std::vector<std::pair<std::string, std::string>> &vertices_under_constraint, uint64_t commit_timestamp) {
  if (constraints_.contains(std::make_pair(label, properties))) {
    return false;
  }

  auto disk_transaction = std::unique_ptr<rocksdb::Transaction>(
      kvstore_->db_->BeginTransaction(rocksdb::WriteOptions(), rocksdb::TransactionOptions()));
  for (const auto &[key, value] : vertices_under_constraint) {
    if (!disk_transaction->Put(key, value).ok()) {
      return false;
    }
  }

  if (!CommitWithTimestamp(disk_transaction.get(), commit_timestamp)) {
    return false;
  }
  constraints_.emplace(label, properties);
  gauge_.Increment();
  return true;
}

std::expected<void, ConstraintViolation> DiskUniqueConstraints::Validate(
    const Vertex &vertex, SeenUniqueValues &unique_storage, const std::unordered_set<Gid> &superseded_gids) const {
  if (vertex.deleted()) {
    return {};
  }
  for (const auto &[constraint_label, constraint_properties] : constraints_) {
    if (IsVertexUnderConstraint(vertex, constraint_label, constraint_properties)) {
      if (auto vertex_check_result = TestIfVertexSatisifiesUniqueConstraint(
              vertex, unique_storage, constraint_label, constraint_properties, superseded_gids);
          !vertex_check_result.has_value()) {
        return std::unexpected{vertex_check_result.error()};
      }
    }
  }
  return {};
}

std::expected<void, ConstraintViolation> DiskUniqueConstraints::TestIfVertexSatisifiesUniqueConstraint(
    const Vertex &vertex, SeenUniqueValues &unique_storage, const LabelId &constraint_label,
    const std::set<PropertyId> &constraint_properties, const std::unordered_set<Gid> &superseded_gids) const {
  auto property_values = vertex.properties.ExtractPropertyValues(constraint_properties);

  if (property_values.has_value() && !EveryValueEqualsItself(*property_values)) {
    return {};
  }

  if (property_values.has_value() &&
      VertexIsUnique(
          property_values.value(), unique_storage, constraint_label, constraint_properties, vertex.gid, superseded_gids)) {
    unique_storage.emplace(
        std::make_pair(UniqueConstraintRule{.label = constraint_label, .properties = constraint_properties},
                       std::move(property_values.value())),
        vertex.gid);
    return {};
  }

  return std::unexpected{
      ConstraintViolation{ConstraintViolation::Type::UNIQUE, constraint_label, constraint_properties}};
}

bool DiskUniqueConstraints::VertexIsUnique(const std::vector<PropertyValue> &property_values,
                                           SeenUniqueValues &unique_storage, const LabelId &constraint_label,
                                           const std::set<PropertyId> &constraint_properties, const Gid gid,
                                           const std::unordered_set<Gid> &superseded_gids) const {
  const auto lookup_key = std::make_pair(
      UniqueConstraintRule{.label = constraint_label, .properties = constraint_properties}, property_values);
  if (const auto seen_it = unique_storage.find(lookup_key); seen_it != unique_storage.end()) {
    return seen_it->second == gid;
  }

  auto disk_transaction = CreateAllReadingRocksDBTransaction();

  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction->GetIterator(ro));

  const std::string rule_prefix = utils::GetKeyForUniqueConstraintsDurability(constraint_label, constraint_properties) + "|";
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const std::string key = it->key().ToString();
    if (!key.starts_with(rule_prefix)) {
      continue;
    }
    const Gid stored_gid = Gid::FromString(utils::ExtractGidFromUniqueConstraintStorage(key));
    if (stored_gid == gid || superseded_gids.contains(stored_gid)) {
      continue;
    }
    if (utils::DeserializePropertiesFromUniqueConstraintStorage(it->value().ToString())
            .ExtractPropertyValues(constraint_properties) == property_values) {
      return false;
    }
  }
  if (!it->status().ok()) {
    throw kvstore::KVStoreError("Failed scanning unique constraint storage: {}", it->status().ToString());
  }
  return true;
}

bool DiskUniqueConstraints::StageSyncVertex(rocksdb::Transaction &disk_transaction, const Vertex &vertex) const {
  for (const auto &[constraint_label, constraint_properties] : constraints_) {
    const auto key =
        utils::SerializeVertexAsKeyForUniqueConstraint(constraint_label, constraint_properties, vertex.gid.ToString());
    if (IsVertexUnderConstraint(vertex, constraint_label, constraint_properties)) {
      auto value = utils::SerializeVertexAsValueForUniqueConstraint(constraint_label, vertex.labels, vertex.properties);
      if (!disk_transaction.Put(key, value).ok()) {
        return false;
      }
    } else {
      if (!disk_transaction.Delete(key).ok()) {
        return false;
      }
    }
  }
  return true;
}

bool DiskUniqueConstraints::StageClearDeletedVertex(rocksdb::Transaction &disk_transaction,
                                                    std::string_view gid) const {
  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction.GetIterator(ro));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (std::string key = it->key().ToString(); gid == utils::ExtractGidFromUniqueConstraintStorage(key)) {
      if (!disk_transaction.Delete(key).ok()) {
        return false;
      }
    }
  }
  return it->status().ok();
}

bool DiskUniqueConstraints::StageDeleteRemovedLabels(rocksdb::Transaction &disk_transaction,
                                                     TxConstraintDeletionMap const &tx_entries) const {
  if (tx_entries.empty()) {
    return true;
  }
  return ClearTransactionEntriesWithRemovedConstraintLabel(disk_transaction, tx_entries);
}

bool DiskUniqueConstraints::ClearDeletedVertex(const std::string_view gid,
                                               uint64_t transaction_commit_timestamp) const {
  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  if (!StageClearDeletedVertex(*disk_transaction, gid)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), transaction_commit_timestamp);
}

bool DiskUniqueConstraints::DeleteVerticesWithRemovedConstraintLabel(DeletionEntries &deletion_entries,
                                                                     uint64_t transaction_start_timestamp,
                                                                     uint64_t transaction_commit_timestamp) {
  if (deletion_entries.empty()) {
    return true;
  }
  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  auto tx_it = deletion_entries.find(transaction_start_timestamp);
  if (tx_it == deletion_entries.end()) {
    return true;
  }
  if (!StageDeleteRemovedLabels(*disk_transaction, tx_it->second)) {
    spdlog::error("Deletion of vertices with removed constraint label failed.");
    return false;
  }
  if (!CommitWithTimestamp(disk_transaction.get(), transaction_commit_timestamp)) {
    return false;
  }
  deletion_entries.erase(tx_it);
  return true;
}

bool DiskUniqueConstraints::SyncVertexToUniqueConstraintsStorage(const Vertex &vertex,
                                                                 uint64_t commit_timestamp) const {
  auto disk_transaction = std::unique_ptr<rocksdb::Transaction>(
      kvstore_->db_->BeginTransaction(rocksdb::WriteOptions(), rocksdb::TransactionOptions()));
  if (!StageSyncVertex(*disk_transaction, vertex)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), commit_timestamp);
}

DiskUniqueConstraints::CreationStatus DiskUniqueConstraints::CheckIfConstraintCanBeCreated(
    LabelId label, const std::set<PropertyId> &properties) const {
  if (properties.empty()) {
    return CreationStatus::EMPTY_PROPERTIES;
  }
  if (properties.size() > kUniqueConstraintsMaxProperties) {
    return CreationStatus::PROPERTIES_SIZE_LIMIT_EXCEEDED;
  }
  if (constraints_.contains(std::make_pair(label, properties))) {
    return CreationStatus::ALREADY_EXISTS;
  }
  return CreationStatus::SUCCESS;
}

DiskUniqueConstraints::DeletionStatus DiskUniqueConstraints::DropConstraint(LabelId label,
                                                                            const std::set<PropertyId> &properties,
                                                                            uint64_t commit_timestamp) {
  if (auto drop_properties_check_result = UniqueConstraints::CheckPropertiesBeforeDeletion(properties);
      drop_properties_check_result != UniqueConstraints::DeletionStatus::SUCCESS) {
    return drop_properties_check_result;
  }
  if (!constraints_.contains({label, properties})) {
    return UniqueConstraints::DeletionStatus::NOT_FOUND;
  }

  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction->GetIterator(ro));
  const std::string rule_prefix = utils::GetKeyForUniqueConstraintsDurability(label, properties) + "|";
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.starts_with(rule_prefix)) {
      if (!disk_transaction->Delete(key).ok()) {
        return UniqueConstraints::DeletionStatus::NOT_FOUND;
      }
    }
  }
  if (!it->status().ok() || !CommitWithTimestamp(disk_transaction.get(), commit_timestamp)) {
    return UniqueConstraints::DeletionStatus::NOT_FOUND;
  }

  constraints_.erase({label, properties});
  gauge_.Decrement();
  return UniqueConstraints::DeletionStatus::SUCCESS;
}

void DiskUniqueConstraints::ActiveConstraints::UpdateOnRemoveLabel(LabelId removed_label,
                                                                   const Vertex &vertex_before_update,
                                                                   uint64_t transaction_start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  for (const auto &constraint : constraints_->constraints_) {
    if (constraint.first == removed_label) {
      const auto &[constraint_label, constraint_properties] = constraint;
      entries_for_deletion[transaction_start_timestamp][vertex_before_update.gid].emplace(constraint_label,
                                                                                          constraint_properties);
    }
  }
}

void DiskUniqueConstraints::ActiveConstraints::UpdateOnAddLabel(LabelId added_label, const Vertex &vertex_before_update,
                                                                uint64_t transaction_start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  if (auto tx_it = entries_for_deletion.find(transaction_start_timestamp); tx_it != entries_for_deletion.end()) {
    if (auto vertex_constraints_it = tx_it->second.find(vertex_before_update.gid);
        vertex_constraints_it != tx_it->second.end()) {
      std::erase_if(vertex_constraints_it->second,
                    [added_label](const auto &constraint) { return constraint.first == added_label; });
    }
  }
}

void DiskUniqueConstraints::Clear() {
  const size_t removed = constraints_.size();
  constraints_.clear();
  for (size_t i = 0; i < removed; ++i) {
    gauge_.Decrement();
  }

  auto disk_transaction = CreateAllReadingRocksDBTransaction();

  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction->GetIterator(ro));

  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    (void)disk_transaction->Delete(it->key().ToString());
  }

  (void)CommitWithTimestamp(disk_transaction.get(), 0);
}

RocksDBStorage *DiskUniqueConstraints::GetRocksDBStorage() const { return kvstore_.get(); }

void DiskUniqueConstraints::LoadUniqueConstraints(const std::vector<std::string> &keys) {
  auto const parse_strict_u32 = [](std::string_view raw, std::string_view record) -> uint32_t {
    if (raw.empty()) {
      throw utils::BasicException("Corrupted unique-constraint metadata record '{}': empty identifier.", record);
    }
    uint32_t parsed{0U};
    const char *const begin = raw.data();
    const char *const end = begin + raw.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
      throw utils::BasicException("Corrupted unique-constraint metadata record '{}': invalid numeric identifier '{}'.",
                                  record, raw);
    }
    return parsed;
  };

  for (const auto &key : keys) {
    if (key.empty()) continue;
    std::vector<std::string> key_parts = utils::Split(key, ",");
    if (key_parts.size() < 2) {
      throw utils::BasicException(
          "Corrupted unique-constraint metadata record '{}': expected '<label_id>,<property_id>...'.", key);
    }
    LabelId label = LabelId::FromUint(parse_strict_u32(key_parts[0], key));
    std::set<PropertyId> properties;
    for (size_t i = 1; i < key_parts.size(); ++i) {
      properties.insert(PropertyId::FromUint(parse_strict_u32(key_parts[i], key)));
    }
    if (constraints_.emplace(label, std::move(properties)).second) {
      gauge_.Increment();
    }
  }
}

}  // namespace memgraph::storage
