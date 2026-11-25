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

#include <charconv>
#include <rocksdb/options.h>
#include <rocksdb/utilities/transaction.h>

#include "storage/v2/disk/delta_utils.hpp"
#include "storage/v2/disk/label_index.hpp"
#include "storage/v2/indices/active_indices_updater.hpp"
#include "storage/v2/transaction.hpp"
#include "utils/exceptions.hpp"
#include "utils/file.hpp"
#include "utils/rocksdb_serialization.hpp"

namespace memgraph::storage {

namespace {

[[nodiscard]] bool ClearTransactionEntriesWithRemovedIndexingLabel(
    rocksdb::Transaction &disk_transaction, const std::map<Gid, std::vector<LabelId>> &transaction_entries) {
  for (const auto &[vertex_gid, labels] : transaction_entries) {
    for (const auto &indexing_label : labels) {
      if (auto status = disk_transaction.Delete(utils::SerializeVertexAsKeyForLabelIndex(indexing_label, vertex_gid));
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

DiskLabelIndex::DiskLabelIndex(const Config &config) {
  utils::EnsureDirOrDie(config.disk.label_index_directory);
  kvstore_ = std::make_unique<RocksDBStorage>();
  kvstore_->options_.create_if_missing = true;
  kvstore_->InstallComparator(std::make_unique<ComparatorWithU64TsImpl>());
  logging::AssertRocksDBStatus(rocksdb::TransactionDB::Open(
      kvstore_->options_, rocksdb::TransactionDBOptions(), config.disk.label_index_directory, &kvstore_->db_));
}

auto DiskLabelIndex::GetActiveIndices() const -> std::shared_ptr<LabelIndex::ActiveIndices> {
  return std::make_shared<DiskLabelIndex::ActiveIndices>(index_);
}

void DiskLabelIndex::ActiveIndices::AbortEntries(AbortableInfo const &, uint64_t start_timestamp) {
  ClearEntriesForTransaction(start_timestamp);
}

LabelIndex::AbortProcessor DiskLabelIndex::ActiveIndices::GetAbortProcessor() const { return AbortProcessor{}; }

DiskLabelIndex::EntriesForDeletion DiskLabelIndex::ActiveIndices::ExtractEntriesForTransaction(
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

void DiskLabelIndex::ActiveIndices::ClearEntriesForTransaction(uint64_t start_timestamp) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  entries_for_deletion_.erase(start_timestamp);
}

bool DiskLabelIndex::CreateIndex(LabelId label, const std::vector<std::pair<std::string, std::string>> &vertices,
                                 uint64_t commit_timestamp) {
  if (index_.contains(label)) {
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
  index_.emplace(label);
  return true;
}

std::unique_ptr<rocksdb::Transaction> DiskLabelIndex::CreateRocksDBTransaction() const {
  return std::unique_ptr<rocksdb::Transaction>(
      kvstore_->db_->BeginTransaction(rocksdb::WriteOptions(), rocksdb::TransactionOptions()));
}

std::unique_ptr<rocksdb::Transaction> DiskLabelIndex::CreateAllReadingRocksDBTransaction() const {
  auto tx = CreateRocksDBTransaction();
  tx->SetReadTimestampForValidation(std::numeric_limits<uint64_t>::max());
  return tx;
}

bool DiskLabelIndex::CommitTransaction(rocksdb::Transaction *disk_transaction, uint64_t commit_timestamp) {
  return CommitWithTimestamp(disk_transaction, commit_timestamp);
}

bool DiskLabelIndex::StageSyncVertex(rocksdb::Transaction &disk_transaction, const Vertex &vertex) const {
  for (const LabelId index_label : index_) {
    const auto key = utils::SerializeVertexAsKeyForLabelIndex(index_label, vertex.gid);
    if (!std::ranges::contains(vertex.labels, index_label)) {
      if (!disk_transaction.Delete(key).ok()) {
        return false;
      }
      continue;
    }
    if (!disk_transaction
             .Put(key, utils::SerializeVertexAsValueForLabelIndex(index_label, vertex.labels, vertex.properties))
             .ok()) {
      return false;
    }
  }
  return true;
}

bool DiskLabelIndex::StageClearDeletedVertex(rocksdb::Transaction &disk_transaction, std::string_view gid) const {
  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction.GetIterator(ro));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    if (std::string key = it->key().ToString(); gid == utils::ExtractGidFromLabelIndexStorage(key)) {
      if (!disk_transaction.Delete(key).ok()) {
        return false;
      }
    }
  }
  return it->status().ok();
}

bool DiskLabelIndex::StageDeleteRemovedLabels(rocksdb::Transaction &disk_transaction,
                                              EntriesForDeletion const &entries_for_deletion) const {
  if (entries_for_deletion.empty()) {
    return true;
  }
  return ClearTransactionEntriesWithRemovedIndexingLabel(disk_transaction, entries_for_deletion);
}

bool DiskLabelIndex::SyncVertexToLabelIndexStorage(const Vertex &vertex, uint64_t commit_timestamp) const {
  auto disk_transaction = CreateRocksDBTransaction();
  if (!StageSyncVertex(*disk_transaction, vertex)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), commit_timestamp);
}

bool DiskLabelIndex::ClearDeletedVertex(std::string_view gid, uint64_t transaction_commit_timestamp) const {
  auto disk_transaction = CreateAllReadingRocksDBTransaction();
  if (!StageClearDeletedVertex(*disk_transaction, gid)) {
    return false;
  }
  return CommitWithTimestamp(disk_transaction.get(), transaction_commit_timestamp);
}

bool DiskLabelIndex::DeleteVerticesWithRemovedIndexingLabel(uint64_t /*transaction_start_timestamp*/,
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

void DiskLabelIndex::ActiveIndices::UpdateOnAddLabel(LabelId added_label, Vertex *vertex_before_update,
                                                     const Transaction &tx) {
  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  auto tx_it = entries_for_deletion_.find(tx.start_timestamp);
  if (tx_it == entries_for_deletion_.end()) {
    return;
  }
  auto vertex_label_index_it = tx_it->second.find(vertex_before_update->gid);
  if (vertex_label_index_it == tx_it->second.end()) {
    return;
  }
  std::erase_if(vertex_label_index_it->second, [added_label](LabelId index) { return index == added_label; });
}

void DiskLabelIndex::ActiveIndices::UpdateOnRemoveLabel(LabelId removed_label, Vertex *vertex_before_update,
                                                        const Transaction &tx) {
  if (!index_.contains(removed_label)) return;

  std::lock_guard<std::mutex> const lock{deletion_mutex_};
  auto &per_tx = entries_for_deletion_[tx.start_timestamp];
  per_tx[vertex_before_update->gid].emplace_back(removed_label);
}

bool DiskLabelIndex::DropIndex(LabelId label, ActiveIndicesUpdater const &updater, uint64_t commit_timestamp) {
  if (!index_.contains(label)) {
    return false;
  }
  auto disk_transaction = CreateAllReadingRocksDBTransaction();

  rocksdb::ReadOptions ro;
  std::string strTs = utils::StringTimestamp(std::numeric_limits<uint64_t>::max());
  rocksdb::Slice ts(strTs);
  ro.timestamp = &ts;
  auto it = std::unique_ptr<rocksdb::Iterator>(disk_transaction->GetIterator(ro));
  const std::string serialized_label_prefix = utils::SerializeLabelIndexPrefix(label);
  const std::string label_str = label.ToString();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    std::string key = it->key().ToString();
    if (key.starts_with(serialized_label_prefix) && utils::FindPartOfStringView(key, '|', 1) == label_str) {
      if (!disk_transaction->Delete(key).ok()) {
        return false;
      }
    }
  }
  if (!it->status().ok()) {
    return false;
  }

  if (!CommitWithTimestamp(disk_transaction.get(), commit_timestamp)) {
    return false;
  }
  index_.erase(label);
  updater(GetActiveIndices());
  return true;
}

bool DiskLabelIndex::ActiveIndices::IndexExists(LabelId label) const { return index_.contains(label); }

bool DiskLabelIndex::ActiveIndices::IndexReady(LabelId label) const { return index_.contains(label); }

std::vector<LabelId> DiskLabelIndex::ActiveIndices::ListIndices(uint64_t /*start_timestamp*/) const {
  return {index_.begin(), index_.end()};
}

uint64_t DiskLabelIndex::ActiveIndices::ApproximateVertexCount(LabelId /*label*/) const { return 10; }

void DiskLabelIndex::LoadIndexInfo(const std::vector<std::string> &labels) {
  for (const std::string &label : labels) {
    if (label.empty()) continue;
    uint32_t parsed{0U};
    const char *const begin = label.data();
    const char *const end = begin + label.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
      throw utils::BasicException("Corrupted disk label-index metadata record '{}': expected numeric label ID.", label);
    }
    index_.insert(LabelId::FromUint(parsed));
  }
}

RocksDBStorage *DiskLabelIndex::GetRocksDBStorage() const { return kvstore_.get(); }

std::unordered_set<LabelId> DiskLabelIndex::GetInfo() const { return index_; }

}  // namespace memgraph::storage
