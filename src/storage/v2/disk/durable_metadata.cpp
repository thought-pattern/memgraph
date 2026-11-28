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
#include <range/v3/all.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/kvstore.hpp"
#include "storage/v2/config.hpp"
#include "storage/v2/disk/durable_metadata.hpp"
#include "utils/file.hpp"
#include "utils/rocksdb_serialization.hpp"
#include "utils/string.hpp"

namespace r = ranges;
namespace rv = r::views;

namespace {
constexpr const char *kLastTransactionStartTimeStamp = "last_transaction_start_timestamp";
constexpr const char *kVertexCountDescr = "vertex_count";
constexpr const char *kEdgeDountDescr = "edge_count";
constexpr const char *kNextVertexIdDescr = "next_vertex_id";
constexpr const char *kNextEdgeIdDescr = "next_edge_id";
constexpr const char *kLabelIndexStr = "label_index";
constexpr const char *kLabelPropertyIndexStr = "label_property_index";
constexpr const char *kTextIndexStr = "text_index";
constexpr const char *kExistenceConstraintsStr = "existence_constraints";
constexpr const char *kUniqueConstraintsStr = "unique_constraints";
}  // namespace

namespace memgraph::storage {

DurableMetadata::DurableMetadata(const Config &config)
    : durability_kvstore_(kvstore::KVStore(config.disk.durability_directory)), config_(config) {
  MG_ASSERT(utils::DirExists(config_.disk.durability_directory),
            "Durability directory for saving disk metadata does not exist.");
}

DurableMetadata::DurableMetadata(DurableMetadata &&other) noexcept
    : durability_kvstore_(std::move(other.durability_kvstore_)), config_(std::move(other.config_)) {}

bool DurableMetadata::UpdateMetaData(uint64_t timestamp, uint64_t vertex_count, uint64_t edge_count,
                                     uint64_t next_vertex_id, uint64_t next_edge_id) {
  return durability_kvstore_.PutMultiple({
      {kLastTransactionStartTimeStamp, std::to_string(timestamp)},
      {kVertexCountDescr, std::to_string(vertex_count)},
      {kEdgeDountDescr, std::to_string(edge_count)},
      {kNextVertexIdDescr, std::to_string(next_vertex_id)},
      {kNextEdgeIdDescr, std::to_string(next_edge_id)},
  });
}

std::optional<uint64_t> DurableMetadata::LoadTimestampIfExists() const {
  return LoadPropertyIfExists(kLastTransactionStartTimeStamp);
}

std::optional<uint64_t> DurableMetadata::LoadVertexCountIfExists() const {
  return LoadPropertyIfExists(kVertexCountDescr);
}

std::optional<uint64_t> DurableMetadata::LoadEdgeCountIfExists() const { return LoadPropertyIfExists(kEdgeDountDescr); }

std::optional<uint64_t> DurableMetadata::LoadNextVertexIdIfExists() const {
  return LoadPropertyIfExists(kNextVertexIdDescr);
}

std::optional<uint64_t> DurableMetadata::LoadNextEdgeIdIfExists() const {
  return LoadPropertyIfExists(kNextEdgeIdDescr);
}

std::optional<uint64_t> DurableMetadata::LoadPropertyIfExists(const std::string &property) const {
  auto count = durability_kvstore_.GetStrict(property);
  if (!count.has_value()) {
    if (count.error() == kvstore::KVStore::GetError::kNotFound) {
      return std::nullopt;
    }
    throw kvstore::KVStoreError("Failed to read durable disk metadata key '{}'.", property);
  }
  const std::string &last_count = count.value();
  if (last_count.empty()) {
    throw kvstore::KVStoreError("Corrupted durable disk metadata key '{}': empty numeric value.", property);
  }
  uint64_t count_to_return{0U};
  const char *const begin = last_count.data();
  const char *const end = begin + last_count.size();
  const auto [ptr, ec] = std::from_chars(begin, end, count_to_return);
  if (ec != std::errc{} || ptr != end) {
    throw kvstore::KVStoreError("Corrupted durable disk metadata key '{}': invalid integer '{}'.", property,
                                last_count);
  }
  return count_to_return;
}

std::optional<std::vector<std::string>> DurableMetadata::LoadLabelIndexInfoIfExists() const {
  return LoadInfoFromAuxiliaryStorages(kLabelIndexStr);
}

std::optional<std::vector<std::string>> DurableMetadata::LoadLabelPropertyIndexInfoIfExists() const {
  return LoadInfoFromAuxiliaryStorages(kLabelPropertyIndexStr);
}

std::optional<std::vector<std::string>> DurableMetadata::LoadTextIndexInfoIfExists() const {
  return LoadInfoFromAuxiliaryStorages(kTextIndexStr);
}

std::optional<std::vector<std::string>> DurableMetadata::LoadExistenceConstraintInfoIfExists() const {
  return LoadInfoFromAuxiliaryStorages(kExistenceConstraintsStr);
}

std::optional<std::vector<std::string>> DurableMetadata::LoadUniqueConstraintInfoIfExists() const {
  return LoadInfoFromAuxiliaryStorages(kUniqueConstraintsStr);
}

std::optional<std::vector<std::string>> DurableMetadata::LoadInfoFromAuxiliaryStorages(
    const std::string &property) const {
  auto maybe_props = durability_kvstore_.GetStrict(property);
  if (!maybe_props.has_value()) {
    if (maybe_props.error() == kvstore::KVStore::GetError::kNotFound) {
      return std::nullopt;
    }
    throw kvstore::KVStoreError("Failed to read durable disk metadata key '{}'.", property);
  }
  if (maybe_props->empty()) {
    return std::vector<std::string>{};
  }
  return utils::Split(maybe_props.value(), "|");
}

bool DurableMetadata::PersistLabelIndexCreation(LabelId label) {
  const auto serialized_label = label.ToString();
  auto label_index_store = durability_kvstore_.GetStrict(kLabelIndexStr);
  if (label_index_store.has_value()) {
    std::string &value = label_index_store.value();
    value += "|";
    value += serialized_label;
    return durability_kvstore_.Put(kLabelIndexStr, value);
  }
  if (label_index_store.error() == kvstore::KVStore::GetError::kReadError) {
    return false;
  }
  return durability_kvstore_.Put(kLabelIndexStr, serialized_label);
}

bool DurableMetadata::PersistLabelIndexDeletion(LabelId label) {
  const auto serialized_label = label.ToString();
  auto label_index_store = durability_kvstore_.GetStrict(kLabelIndexStr);
  if (label_index_store.has_value()) {
    const std::string &value = label_index_store.value();
    std::vector<std::string> labels = utils::Split(value, "|");
    std::erase(labels, serialized_label);
    if (labels.empty()) {
      return durability_kvstore_.Delete(kLabelIndexStr);
    }
    return durability_kvstore_.Put(kLabelIndexStr, utils::Join(labels, "|"));
  }
  return label_index_store.error() != kvstore::KVStore::GetError::kReadError;
}

bool DurableMetadata::PersistLabelPropertyIndexAndExistenceConstraintCreation(LabelId label, PropertyId property,
                                                                              const std::string &key) {
  const std::string label_property_pair = label.ToString() + "," + property.ToString();
  auto label_property_index_store = durability_kvstore_.GetStrict(key);
  if (label_property_index_store.has_value()) {
    std::string &value = label_property_index_store.value();
    value += "|";
    value += label_property_pair;
    return durability_kvstore_.Put(key, value);
  }
  if (label_property_index_store.error() == kvstore::KVStore::GetError::kReadError) {
    return false;
  }
  return durability_kvstore_.Put(key, label_property_pair);
}

bool DurableMetadata::PersistLabelPropertyIndexAndExistenceConstraintDeletion(LabelId label, PropertyId property,
                                                                              const std::string &key) {
  const std::string label_property_pair = label.ToString() + "," + property.ToString();
  auto label_property_index_store = durability_kvstore_.GetStrict(key);
  if (label_property_index_store.has_value()) {
    const std::string &value = label_property_index_store.value();
    std::vector<std::string> label_properties = utils::Split(value, "|");
    std::erase(label_properties, label_property_pair);
    if (label_properties.empty()) {
      return durability_kvstore_.Delete(key);
    }
    return durability_kvstore_.Put(key, utils::Join(label_properties, "|"));
  }
  return label_property_index_store.error() != kvstore::KVStore::GetError::kReadError;
}

bool DurableMetadata::PersistTextIndexCreation(const storage::TextIndexSpec &text_index) {
  const auto properties_str = utils::Join(
      text_index.properties | rv::transform([](const auto &property_id) { return property_id.ToString(); }), ",");
  const auto index_name_label_properties =
      fmt::format("{},{},{}", text_index.index_name, text_index.label.ToString(), properties_str);
  auto text_index_store = durability_kvstore_.GetStrict(kTextIndexStr);
  if (text_index_store.has_value()) {
    auto &value = text_index_store.value();
    value = fmt::format("{}|{}", value, index_name_label_properties);
    return durability_kvstore_.Put(kTextIndexStr, value);
  }
  if (text_index_store.error() == kvstore::KVStore::GetError::kReadError) {
    return false;
  }
  return durability_kvstore_.Put(kTextIndexStr, index_name_label_properties);
}

bool DurableMetadata::PersistTextIndexDeletion(std::string_view index_name) {
  auto text_index_store = durability_kvstore_.GetStrict(kTextIndexStr);
  if (text_index_store.has_value()) {
    const std::string &value = text_index_store.value();
    std::vector<std::string> text_indices = utils::Split(value, "|");
    std::erase_if(text_indices, [index_name](const std::string &entry) {
      const auto comma_pos = entry.find(',');
      const std::string_view entry_name =
          comma_pos == std::string::npos ? std::string_view{entry} : std::string_view{entry}.substr(0, comma_pos);
      return entry_name == index_name;
    });
    if (text_indices.empty()) {
      return durability_kvstore_.Delete(kTextIndexStr);
    }
    return durability_kvstore_.Put(kTextIndexStr, utils::Join(text_indices, "|"));
  }
  return text_index_store.error() != kvstore::KVStore::GetError::kReadError;
}

bool DurableMetadata::PersistUniqueConstraintCreation(LabelId label, const std::set<PropertyId> &properties) {
  const std::string entry = utils::GetKeyForUniqueConstraintsDurability(label, properties);

  auto unique_store = durability_kvstore_.GetStrict(kUniqueConstraintsStr);
  if (unique_store.has_value()) {
    std::string &value = unique_store.value();
    value += "|" + entry;
    return durability_kvstore_.Put(kUniqueConstraintsStr, value);
  }
  if (unique_store.error() == kvstore::KVStore::GetError::kReadError) {
    return false;
  }
  return durability_kvstore_.Put(kUniqueConstraintsStr, entry);
}

bool DurableMetadata::PersistUniqueConstraintDeletion(LabelId label, const std::set<PropertyId> &properties) {
  const std::string entry = utils::GetKeyForUniqueConstraintsDurability(label, properties);

  auto unique_store = durability_kvstore_.GetStrict(kUniqueConstraintsStr);
  if (unique_store.has_value()) {
    const std::string &value = unique_store.value();
    std::vector<std::string> unique_constraints = utils::Split(value, "|");
    std::erase(unique_constraints, entry);
    if (unique_constraints.empty()) {
      return durability_kvstore_.Delete(kUniqueConstraintsStr);
    }
    return durability_kvstore_.Put(kUniqueConstraintsStr, utils::Join(unique_constraints, "|"));
  }
  return unique_store.error() != kvstore::KVStore::GetError::kReadError;
}

}  // namespace memgraph::storage
