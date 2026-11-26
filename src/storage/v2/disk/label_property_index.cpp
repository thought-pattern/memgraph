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

#include "storage/v2/disk/label_property_index.hpp"
#include <charconv>
#include <range/v3/all.hpp>
#include "storage/v2/disk/delta_utils.hpp"
#include "storage/v2/indices/active_indices_updater.hpp"
#include "storage/v2/transaction.hpp"
#include "utils/exceptions.hpp"
#include "utils/file.hpp"
#include "utils/logging.hpp"
#include "utils/rocksdb_serialization.hpp"

namespace memgraph::storage {

namespace {

bool IsVertexIndexedByLabelProperty(const Vertex &vertex, LabelId label, PropertyId property) {
  return std::ranges::contains(vertex.labels, label) && vertex.properties.HasProperty(property);
}

[[nodiscard]] bool ClearTransactionEntriesWithRemovedIndexingLabel(
    rocksdb::Transaction &disk_transaction, const DiskLabelPropertyIndex::EntriesForDeletion &transaction_entries) {
  for (const auto &[vertex_gid, index] : transaction_entries) {
    for (const auto &[indexing_label, indexing_property] : index) {
      if (auto status = disk_transaction.Delete(
              utils::SerializeVertexAsKeyForLabelPropertyIndex(indexing_label, indexing_property, vertex_gid));
          !status.ok()) {
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

DiskLabelPropertyIndex::DiskLabelPropertyIndex(const Config &config) {
  utils::EnsureDirOrDie(config.disk.label_property_index_directory);
  kvstore_ = std::make_unique<RocksDBStorage>();
  kvstore_->options_.create_if_missing = true;
  kvstore_->InstallComparator(std::make_unique<ComparatorWithU64TsImpl>());
  logging::AssertRocksDBStatus(rocksdb::TransactionDB::Open(
      kvstore_->options_, rocksdb::TransactionDBOptions(), config.disk.label_property_index_directory, &kvstore_->db_));
}

bool DiskLabelPropertyIndex::CreateIndex(LabelId label, PropertyId property,
                                         const std::vector<std::pair<std::string, std::string>> &vertices,
                                         uint64_t commit_timestamp) {
  if (index_.contains({label, property})) {
    return false;
  }

  auto disk_transaction = CreateRocksDBTransaction();
  for (const auto &[key, value] : vertices) {
    if (!disk_transaction->Put(key, value).ok()) {
      return false;
    }
  }

  if (!CommitWithTimestamp(disk_transaction.get(), commit_timestamp)) {
    return false;
  }
  index_.emplace(label, property);
  return true;
}

std::unique_ptr<rocksdb::Transaction> DiskLabelPropertyIndex::CreateRocksDBTransaction() const {
  return std::unique_ptr<rocksdb::Transaction>(
      kvstore_->db_->BeginTransaction(rocksdb::WriteOptions(), rocksdb::TransactionOptions()));
}

std::unique_ptr<rocksdb::Transaction> DiskLabelPropertyIndex::CreateAllReadingRocksDBTransaction() const {
  auto tx = CreateRocksDBTransaction();
  tx->SetReadTimestampForValidation(std::numeric_limits<uint64_t>::max());
  return tx;
}

bool DiskLabelPropertyIndex::CommitTransaction(rocksdb::Transaction *disk_transaction, uint64_t commit_timestamp) {
  return CommitWithTimestamp(disk_transaction, commit_timestamp);
}

bool DiskLabelPropertyIndex::StageSyncVertex(rocksdb::Transaction &disk_transaction, const Vertex &vertex) const {
  for (const auto &[index_label, index_property] : index_) {
    const auto key = utils::SerializeVertexAsKeyForLabelPropertyIndex(index_label, index_property, vertex.gid);
    if (IsVertexIndexedByLabelProperty(vertex, index_label, index_property)) {
      if (!disk_transaction
               .Put(key,
                    utils::SerializeVertexAsValueForLabelPropertyIndex(index_label, vertex.labels, vertex.properties))
               .ok()) {
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

bool DiskLabelPropertyIndex::StageClearDeletedVertex(rocksdb::Transaction &disk_transaction,
                                                     std::string_view gid) const {
  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction.GetIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (std::string key = it->key().ToString(); gid == utils::ExtractGidFromLabelPropertyIndexStorage(key)) {
      if (!disk_transaction.Delete(key).ok()) {
        return false;
      }
    }
  }
  return it->status().ok();
}

bool DiskLabelPropertyIndex::StageDeleteRemovedLabels(rocksdb::Transaction &disk_transaction,
                                                      EntriesForDeletion const &entries_for_deletion) const {
  if (entries_for_deletion.empty()) {
    return true;
  }
  return ClearTransactionEntriesWithRemovedIndexingLabel(disk_transaction, entries_for_deletion);
}

bool DiskLabelPropertyIndex::SyncVertexToLabelPropertyIndexStorage(const Vertex &vertex,
                                                                   uint64_t commit_timestamp) const {
  auto disk_transaction = CreateRocksDBTransaction();
  if (!StageSyncVertex(*disk_transaction, vertex)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), commit_timestamp);
}

bool DiskLabelPropertyIndex::ClearDeletedVertex(std::string_view gid, uint64_t transaction_commit_timestamp) const {
  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  if (!StageClearDeletedVertex(*disk_transaction, gid)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), transaction_commit_timestamp);
}

bool DiskLabelPropertyIndex::DeleteVerticesWithRemovedIndexingLabel(uint64_t /*transaction_start_timestamp*/,
                                                                    uint64_t transaction_commit_timestamp,
                                                                    EntriesForDeletion const &entries_for_deletion) {
  if (entries_for_deletion.empty()) {
    return true;
  }
  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  if (!StageDeleteRemovedLabels(*disk_transaction, entries_for_deletion)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), transaction_commit_timestamp);
}

DiskLabelPropertyIndex::EntriesForDeletion DiskLabelPropertyIndex::ActiveIndices::ExtractEntriesForTransaction(
    uint64_t start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  auto it = entries_for_deletion_.find(start_timestamp);
  if (it == entries_for_deletion_.end()) {
    return {};
  }
  auto extracted = std::move(it->second);
  entries_for_deletion_.erase(it);
  return extracted;
}

void DiskLabelPropertyIndex::ActiveIndices::ClearEntriesForTransaction(uint64_t start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  entries_for_deletion_.erase(start_timestamp);
}

void DiskLabelPropertyIndex::ActiveIndices::UpdateOnAddLabel(LabelId added_label, Vertex *vertex_after_update,
                                                             const Transaction &tx) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  auto tx_it = entries_for_deletion_.find(tx.start_timestamp);
  if (tx_it == entries_for_deletion_.end()) {
    return;
  }
  auto vertex_label_index_it = tx_it->second.find(vertex_after_update->gid);
  if (vertex_label_index_it == tx_it->second.end()) {
    return;
  }
  std::erase_if(vertex_label_index_it->second,
                [added_label](const LabelProperty &index) { return index.label == added_label; });
}

void DiskLabelPropertyIndex::ActiveIndices::UpdateOnRemoveLabel(LabelId removed_label, Vertex *vertex_after_update,
                                                                const Transaction &tx) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  for (const auto &entry : index_) {
    if (entry.label != removed_label) {
      continue;
    }
    entries_for_deletion_[tx.start_timestamp][vertex_after_update->gid].emplace_back(entry);
  }
}

LabelPropertyIndex::DropResult DiskLabelPropertyIndex::DropIndex(LabelId label,
                                                                 std::vector<PropertyPath> const &properties,
                                                                 ActiveIndicesUpdater const &updater,
                                                                 std::optional<IndexOrder> order,
                                                                 uint64_t commit_timestamp) {
  // Disk only has ASC single top-level property indices; a DESC-only or composite/nested drop is a miss.
  if (order.has_value() && *order != IndexOrder::ASC) return {};
  if (properties.size() != 1 || properties[0].size() != 1) return {};
  const PropertyId property = properties[0][0];
  if (!index_.contains({label, property})) return {};

  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction->GetIterator(ro));
  const std::string prefix = utils::SerializeLabelPropertyIndexPrefix(label, property);
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.starts_with(prefix)) {
      if (!disk_transaction->Delete(key).ok()) {
        return {};
      }
    }
  }
  if (!it->status().ok() || !CommitWithTimestamp(disk_transaction.get(), commit_timestamp)) {
    return {};
  }

  index_.erase({label, property});
  updater(GetActiveIndices());
  return {.dropped_asc = true};
}

bool DiskLabelPropertyIndex::ActiveIndices::IndexExists(LabelId label, std::span<PropertyPath const> properties) const {
  if (properties.size() != 1 || properties[0].size() != 1) return false;
  return index_.contains(LabelProperty{label, properties[0][0]});
}

bool DiskLabelPropertyIndex::ActiveIndices::IndexReady(LabelId label, std::span<PropertyPath const> properties) const {
  if (properties.size() != 1 || properties[0].size() != 1) return false;
  return index_.contains(LabelProperty{label, properties[0][0]});
}

bool DiskLabelPropertyIndex::ActiveIndices::IndexReady(LabelId label, std::span<PropertyPath const> properties,
                                                       IndexOrder order) const {
  return order == IndexOrder::ASC && IndexReady(label, properties);
}

auto DiskLabelPropertyIndex::ActiveIndices::ListIndices(uint64_t /*start_timestamp*/) const
    -> std::vector<LabelPropertyIndexEntry> {
  auto const convert = [](auto &&index) -> LabelPropertyIndexEntry {
    auto [label, property] = index;
    return {label, {PropertyPath{property}}, IndexOrder::ASC};
  };

  return index_ | ranges::views::transform(convert) | ranges::to_vector;
}

auto DiskLabelPropertyIndex::ActiveIndices::ApproximateVertexCount(LabelId /*label*/,
                                                                   std::span<PropertyPath const> /*properties*/) const
    -> uint64_t {
  return 10;
}

auto DiskLabelPropertyIndex::ActiveIndices::ApproximateVertexCount(LabelId /*label*/,
                                                                   std::span<PropertyPath const> /*properties*/,
                                                                   std::span<PropertyValue const> /*values*/) const
    -> uint64_t {
  return 10;
}

auto DiskLabelPropertyIndex::ActiveIndices::ApproximateVertexCount(LabelId /*label*/,
                                                                   std::span<PropertyPath const> /*properties*/,
                                                                   std::span<PropertyValueRange const> /*bounds*/) const
    -> uint64_t {
  return 10;
}

void DiskLabelPropertyIndex::LoadIndexInfo(const std::vector<std::string> &keys) {
  auto const parse_strict_u32 = [](std::string_view raw, std::string_view record) -> uint32_t {
    if (raw.empty()) {
      throw utils::BasicException(
          "Corrupted disk label-property index metadata record '{}': expected '<label_id>,<property_id>'.", record);
    }
    uint32_t parsed{0U};
    const char *const begin = raw.data();
    const char *const end = begin + raw.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
      throw utils::BasicException(
          "Corrupted disk label-property index metadata record '{}': invalid numeric identifier '{}'.", record, raw);
    }
    return parsed;
  };

  for (const auto &label_property : keys) {
    if (label_property.empty()) continue;
    std::vector<std::string> label_property_split = utils::Split(label_property, ",");
    if (label_property_split.size() != 2) {
      throw utils::BasicException(
          "Corrupted disk label-property index metadata record '{}': expected '<label_id>,<property_id>'.",
          label_property);
    }
    index_.emplace(LabelId::FromUint(parse_strict_u32(label_property_split[0], label_property)),
                   PropertyId::FromUint(parse_strict_u32(label_property_split[1], label_property)));
  }
}

RocksDBStorage *DiskLabelPropertyIndex::GetRocksDBStorage() const { return kvstore_.get(); }

auto DiskLabelPropertyIndex::GetInfo() const -> std::set<DiskLabelPropertyIndex::LabelProperty> { return index_; }

auto DiskLabelPropertyIndex::GetActiveIndices() const -> std::shared_ptr<LabelPropertyIndex::ActiveIndices> {
  return std::make_shared<DiskLabelPropertyIndex::ActiveIndices>(index_);
}

auto DiskLabelPropertyIndex::ActiveIndices::RelevantLabelPropertiesIndicesInfo(
    std::span<LabelId const> labels, std::span<PropertyPath const> properties) const
    -> std::vector<LabelPropertiesIndicesInfo> {
  auto res = std::vector<LabelPropertiesIndicesInfo>{};
  // NOTE: only looking for singular property index, as disk does not support composite indices
  for (auto &&[l_pos, label] : ranges::views::enumerate(labels)) {
    for (auto [p_pos, property] : ranges::views::enumerate(properties)) {
      if (IndexReady(label, std::array{property})) {
        res.emplace_back(l_pos, std::vector<int64_t>{static_cast<int64_t>(p_pos)}, label, std::vector{property});
      }
    }
  }
  return res;
}

void DiskLabelPropertyIndex::ActiveIndices::AbortEntries(AbortableInfo const & /*info*/, uint64_t start_timestamp) {
  ClearEntriesForTransaction(start_timestamp);
}

LabelPropertyIndex::AbortProcessor DiskLabelPropertyIndex::ActiveIndices::GetAbortProcessor() const {
  return AbortProcessor();
}

}  // namespace memgraph::storage
