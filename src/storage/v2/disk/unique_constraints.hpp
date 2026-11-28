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

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <unordered_set>

#include "metrics/metric_handles.hpp"
#include "storage/v2/config.hpp"
#include "storage/v2/constraints/active_constraints.hpp"
#include "storage/v2/constraints/unique_constraints.hpp"
#include "storage/v2/disk/rocksdb_storage.hpp"
#include "storage/v2/id_types.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/transaction.hpp"
#include "utils/rocksdb_serialization.hpp"
#include "utils/synchronized.hpp"

namespace memgraph::storage {

class DiskUniqueConstraints : public UniqueConstraints {
 public:
  using TxConstraintDeletionMap = std::map<Gid, std::set<std::pair<LabelId, std::set<PropertyId>>>>;
  using DeletionEntries = std::map<uint64_t, TxConstraintDeletionMap>;

  struct UniqueConstraintRule {
    LabelId label;
    std::set<PropertyId> properties;

    friend auto operator<=>(const UniqueConstraintRule &, const UniqueConstraintRule &) = default;
  };

  using SeenUniqueValues = std::map<std::pair<UniqueConstraintRule, std::vector<PropertyValue>>, Gid>;

  /// ActiveConstraints implementation for disk unique constraints.
  class ActiveConstraints final : public UniqueConstraints::ActiveConstraints {
   public:
    explicit ActiveConstraints(const DiskUniqueConstraints *constraints) : constraints_{constraints} {}

    auto ListConstraints(uint64_t start_timestamp) const
        -> std::vector<std::pair<LabelId, std::set<PropertyId>>> override;
    void UpdateBeforeCommit(const Vertex *vertex, const Transaction &tx) override;
    auto GetAbortProcessor() const -> AbortProcessor override;
    void CollectForAbort(AbortProcessor &processor, Vertex const *vertex) const override;
    void AbortEntries(AbortableInfo &&info, uint64_t exact_start_timestamp) override;
    bool empty() const override;

    auto ConstrainedProperties() const -> InterestingProperties override { return InterestingProperties::Everything(); }

    auto ConstrainedLabels() const -> InterestingLabels override { return InterestingLabels::Everything(); }

    void UpdateOnRemoveLabel(LabelId removed_label, const Vertex &vertex_before_update,
                             uint64_t transaction_start_timestamp) override;

    void UpdateOnAddLabel(LabelId added_label, const Vertex &vertex_before_update,
                          uint64_t transaction_start_timestamp) override;

    TxConstraintDeletionMap ExtractEntriesForTransaction(uint64_t transaction_start_timestamp);

    void ClearEntriesForTransaction(uint64_t transaction_start_timestamp);

    const DiskUniqueConstraints *constraints_;
    mutable std::mutex deletion_mutex_;
    DeletionEntries entries_for_deletion;
  };

  auto GetActiveConstraints() const -> std::shared_ptr<UniqueConstraints::ActiveConstraints> override;

  explicit DiskUniqueConstraints(const Config &config, metrics::GaugeHandle gauge = {});

  CreationStatus CheckIfConstraintCanBeCreated(LabelId label, const std::set<PropertyId> &properties) const;

  [[nodiscard]] bool InsertConstraint(
      LabelId label, const std::set<PropertyId> &properties,
      const std::vector<std::pair<std::string, std::string>> &vertices_under_constraint,
      uint64_t commit_timestamp = 0);

  std::expected<void, ConstraintViolation> Validate(
      const Vertex &vertex, SeenUniqueValues &unique_storage,
      const std::unordered_set<Gid> &superseded_gids = {}) const;

  std::unique_ptr<rocksdb::Transaction> CreateAllReadingRocksDBTransaction() const;

  [[nodiscard]] bool StageSyncVertex(rocksdb::Transaction &disk_transaction, const Vertex &vertex) const;

  [[nodiscard]] bool StageClearDeletedVertex(rocksdb::Transaction &disk_transaction, std::string_view gid) const;

  [[nodiscard]] bool StageDeleteRemovedLabels(rocksdb::Transaction &disk_transaction,
                                              TxConstraintDeletionMap const &tx_entries) const;

  [[nodiscard]] static bool CommitTransaction(rocksdb::Transaction *disk_transaction, uint64_t commit_timestamp);

  [[nodiscard]] bool ClearDeletedVertex(std::string_view gid, uint64_t transaction_commit_timestamp) const;

  [[nodiscard]] bool DeleteVerticesWithRemovedConstraintLabel(DeletionEntries &deletion_entries,
                                                              uint64_t transaction_start_timestamp,
                                                              uint64_t transaction_commit_timestamp);

  [[nodiscard]] bool SyncVertexToUniqueConstraintsStorage(const Vertex &vertex, uint64_t commit_timestamp) const;

  DeletionStatus DropConstraint(LabelId label, const std::set<PropertyId> &properties, uint64_t commit_timestamp = 0);

  void Clear() override;

  RocksDBStorage *GetRocksDBStorage() const;

  void LoadUniqueConstraints(const std::vector<std::string> &keys);

 private:
  std::set<std::pair<LabelId, std::set<PropertyId>>> constraints_;
  std::unique_ptr<RocksDBStorage> kvstore_;
  metrics::GaugeHandle gauge_{};

  [[nodiscard]] std::expected<void, ConstraintViolation> TestIfVertexSatisifiesUniqueConstraint(
      const Vertex &vertex, SeenUniqueValues &unique_storage, const LabelId &constraint_label,
      const std::set<PropertyId> &constraint_properties, const std::unordered_set<Gid> &superseded_gids) const;

  bool VertexIsUnique(const std::vector<PropertyValue> &property_values, SeenUniqueValues &unique_storage,
                      const LabelId &constraint_label, const std::set<PropertyId> &constraint_properties, Gid gid,
                      const std::unordered_set<Gid> &superseded_gids) const;
};
}  // namespace memgraph::storage
