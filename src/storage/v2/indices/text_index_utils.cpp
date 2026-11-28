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

#include <filesystem>
#include <fstream>
#include <range/v3/all.hpp>

#include <mgcxx_text_search.hpp>
#include <nlohmann/json.hpp>
#include "storage/v2/exceptions.hpp"
#include "storage/v2/indices/ordered_property_paths.hpp"
#include "storage/v2/indices/property_path.hpp"
#include "storage/v2/indices/text_index_utils.hpp"
#include "storage/v2/name_id_mapper.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/vertex.hpp"
#include "utils/string.hpp"

namespace r = ranges;
namespace rv = r::views;

namespace memgraph::storage {

std::string ToLowerCasePreservingBooleanOperators(std::string_view input) {
  if (input.empty()) return {};

  auto const is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  auto const is_token_delim = [&](char c) { return is_space(c) || c == '(' || c == ')' || c == '"' || c == ':'; };

  std::string result;
  result.reserve(input.length());

  std::size_t pos = 0;
  while (pos < input.size()) {
    const char ch = input[pos];
    if (is_space(ch)) {
      result += ch;
      ++pos;
      continue;
    }
    if (ch == '"') {
      result += ch;
      ++pos;
      while (pos < input.size()) {
        const char qch = input[pos++];
        if (qch == '\\' && pos < input.size()) {
          result += qch;
          result += static_cast<char>(std::tolower(static_cast<unsigned char>(input[pos++])));
          continue;
        }
        if (qch == '"') {
          result += qch;
          break;
        }
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(qch)));
      }
      continue;
    }
    if (ch == '(' || ch == ')' || ch == '+' || ch == '-') {
      result += ch;
      ++pos;
      continue;
    }

    std::size_t end = pos;
    while (end < input.size()) {
      if (input[end] == '\\' && end + 1 < input.size()) {
        end += 2;
        continue;
      }
      if (is_token_delim(input[end])) break;
      ++end;
    }
    const auto token = input.substr(pos, end - pos);
    if (token.empty()) {
      result += input[pos++];
      continue;
    }

    if (end < input.size() && input[end] == ':') {
      // Field qualifier immediately followed by ':' — preserve case-sensitive property spelling.
      result.append(token.data(), token.size());
      result += ':';
      pos = end + 1;
      continue;
    }

    std::size_t ws_after = end;
    while (ws_after < input.size() && is_space(input[ws_after])) {
      ++ws_after;
    }
    if (ws_after > end && ws_after < input.size() && input[ws_after] == ':' &&
        (token.starts_with("data.") || token == kDataField || token == kAllField || token == kGidField ||
         token == kEdgeGidField || token == kFromVertexGidField || token == kToVertexGidField)) {
      result.append(input.data() + pos, ws_after - pos + 1);
      pos = ws_after + 1;
      continue;
    }

    auto uppercase_word = utils::ToUpperCase(token);
    if (uppercase_word == kBooleanAnd || uppercase_word == kBooleanOr || uppercase_word == kBooleanNot) {
      result += uppercase_word;
    } else {
      result += utils::ToLowerCase(token);
    }
    pos = end;
  }

  return result;
}

void ValidateTextIndexName(std::string_view index_name) {
  if (index_name.empty()) {
    throw TextSearchException("Invalid text index name: name must not be empty.");
  }
  if (index_name == "." || index_name == ".." || index_name == kTextIndicesWatermarkFile ||
      index_name == kTextIndicesWatermarkTmpFile) {
    throw TextSearchException("Invalid text index name '{}'.", index_name);
  }
  for (const char ch : index_name) {
    if (ch == '/' || ch == '\\' || ch == '\0') {
      throw TextSearchException("Invalid text index name: path separators and null bytes are forbidden.");
    }
  }
  const std::filesystem::path name_path(index_name);
  if (name_path.is_absolute() || name_path.has_root_path() || name_path.has_parent_path() ||
      std::distance(name_path.begin(), name_path.end()) != 1) {
    throw TextSearchException("Invalid text index name '{}': must be a single path component.", index_name);
  }
}

bool IsValidTextIndexName(std::string_view index_name) {
  try {
    ValidateTextIndexName(index_name);
  } catch (const TextSearchException &) {
    return false;
  }
  return true;
}

std::string MakeIndexPath(const std::filesystem::path &base_path, std::string_view index_name) {
  ValidateTextIndexName(index_name);
  if (base_path.empty()) {
    throw TextSearchException("Text index base directory must not be empty.");
  }
  auto normalized_base = base_path.lexically_normal();
  if (!normalized_base.has_filename() && normalized_base != normalized_base.root_path()) {
    normalized_base = normalized_base.parent_path();
  }
  const auto candidate = (normalized_base / std::filesystem::path(index_name)).lexically_normal();
  const auto rel = candidate.lexically_relative(normalized_base);
  if (rel.empty() || rel == "." || rel == ".." || rel.is_absolute() || *rel.begin() == ".." ||
      std::distance(rel.begin(), rel.end()) != 1) {
    throw TextSearchException("Invalid text index path: '{}' escapes text indices directory.", index_name);
  }
  return candidate.string();
}

bool TextIndicesWatermark::ContainsCleanNodeIndex(const TextIndexSpec &spec) const noexcept {
  return std::ranges::contains(clean_node_indices, spec);
}

bool TextIndicesWatermark::ContainsCleanEdgeIndex(const TextEdgeIndexSpec &spec) const noexcept {
  return std::ranges::contains(clean_edge_indices, spec);
}

void WriteTextIndicesWatermark(const std::filesystem::path &text_indices_dir,
                               const TextIndicesWatermark &watermark) noexcept {
  try {
    std::error_code ec;
    std::filesystem::create_directories(text_indices_dir, ec);
    if (ec) return;

    nlohmann::json payload;
    payload["storage_uuid"] = watermark.storage_uuid;
    payload["last_durable_timestamp"] = watermark.last_durable_timestamp;
    payload["num_committed_txns"] = watermark.num_committed_txns;

    nlohmann::json nodes = nlohmann::json::array();
    for (const auto &spec : watermark.clean_node_indices) {
      nlohmann::json props = nlohmann::json::array();
      for (const auto &prop : spec.properties) {
        props.push_back(prop.AsUint());
      }
      nodes.push_back({{"index_name", spec.index_name}, {"scope", spec.label.AsUint()}, {"properties", std::move(props)}});
    }
    payload["clean_node_indices"] = std::move(nodes);

    nlohmann::json edges = nlohmann::json::array();
    for (const auto &spec : watermark.clean_edge_indices) {
      nlohmann::json props = nlohmann::json::array();
      for (const auto &prop : spec.properties) {
        props.push_back(prop.AsUint());
      }
      edges.push_back(
          {{"index_name", spec.index_name}, {"scope", spec.edge_type.AsUint()}, {"properties", std::move(props)}});
    }
    payload["clean_edge_indices"] = std::move(edges);

    const auto final_path = text_indices_dir / kTextIndicesWatermarkFile;
    const auto tmp_path = text_indices_dir / kTextIndicesWatermarkTmpFile;
    {
      std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) return;
      const auto serialized = payload.dump();
      out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
      out.flush();
      if (!out.good()) {
        out.close();
        std::filesystem::remove(tmp_path, ec);
        return;
      }
    }
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
      std::filesystem::remove(tmp_path, ec);
    }
  } catch (...) {
  }
}

std::optional<TextIndicesWatermark> ReadTextIndicesWatermark(const std::filesystem::path &text_indices_dir) noexcept {
  try {
    const auto final_path = text_indices_dir / kTextIndicesWatermarkFile;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(final_path, ec) || ec) {
      return std::nullopt;
    }
    std::ifstream in(final_path, std::ios::binary);
    if (!in.is_open()) {
      return std::nullopt;
    }
    const auto payload = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/true);
    if (!payload.is_object()) {
      return std::nullopt;
    }

    TextIndicesWatermark watermark;
    watermark.storage_uuid = payload.at("storage_uuid").get<std::string>();
    watermark.last_durable_timestamp = payload.at("last_durable_timestamp").get<uint64_t>();
    watermark.num_committed_txns = payload.at("num_committed_txns").get<uint64_t>();

    for (const auto &entry : payload.at("clean_node_indices")) {
      std::vector<PropertyId> props;
      for (const auto &raw_prop : entry.at("properties")) {
        props.push_back(PropertyId::FromUint(raw_prop.get<uint64_t>()));
      }
      watermark.clean_node_indices.push_back(TextIndexSpec{
          .index_name = entry.at("index_name").get<std::string>(),
          .label = LabelId::FromUint(entry.at("scope").get<uint64_t>()),
          .properties = std::move(props),
      });
    }

    for (const auto &entry : payload.at("clean_edge_indices")) {
      std::vector<PropertyId> props;
      for (const auto &raw_prop : entry.at("properties")) {
        props.push_back(PropertyId::FromUint(raw_prop.get<uint64_t>()));
      }
      watermark.clean_edge_indices.push_back(TextEdgeIndexSpec{
          .index_name = entry.at("index_name").get<std::string>(),
          .edge_type = EdgeTypeId::FromUint(entry.at("scope").get<uint64_t>()),
          .properties = std::move(props),
      });
    }
    return watermark;
  } catch (...) {
    return std::nullopt;
  }
}

void ClearTextIndicesWatermark(const std::filesystem::path &text_indices_dir) noexcept {
  std::error_code ec;
  std::filesystem::remove(text_indices_dir / kTextIndicesWatermarkFile, ec);
  std::filesystem::remove(text_indices_dir / kTextIndicesWatermarkTmpFile, ec);
}

namespace {
// Property types that are indexed in Tantivy are Bool, Int, Double, and String. A value of any
// other type, Null among them, contributes nothing to a document.
void SerializeOne(nlohmann::json &into, NameIdMapper *name_id_mapper, PropertyId prop_id,
                  PropertyValue const &prop_value) {
  switch (prop_value.type()) {
    case PropertyValue::Type::Bool:
      into[name_id_mapper->IdToName(prop_id.AsUint())] = prop_value.ValueBool();
      break;
    case PropertyValue::Type::Int:
      into[name_id_mapper->IdToName(prop_id.AsUint())] = prop_value.ValueInt();
      break;
    case PropertyValue::Type::Double:
      into[name_id_mapper->IdToName(prop_id.AsUint())] = prop_value.ValueDouble();
      break;
    case PropertyValue::Type::String:
      into[name_id_mapper->IdToName(prop_id.AsUint())] = prop_value.ValueString();
      break;
    case PropertyValue::Type::Null:
    case PropertyValue::Type::List:
    case PropertyValue::Type::Map:
    case PropertyValue::Type::TemporalData:
    case PropertyValue::Type::ZonedTemporalData:
    default:
      break;
  }
}

void StringifyOne(std::vector<std::string> &into, PropertyValue const &prop_value) {
  switch (prop_value.type()) {
    case PropertyValue::Type::Bool:
      into.emplace_back(prop_value.ValueBool() ? "true" : "false");
      break;
    case PropertyValue::Type::Int:
      into.push_back(std::to_string(prop_value.ValueInt()));
      break;
    case PropertyValue::Type::Double:
      into.push_back(std::to_string(prop_value.ValueDouble()));
      break;
    case PropertyValue::Type::String:
      into.push_back(prop_value.ValueString());
      break;
    case PropertyValue::Type::Null:
    case PropertyValue::Type::List:
    case PropertyValue::Type::Map:
    case PropertyValue::Type::TemporalData:
    case PropertyValue::Type::ZonedTemporalData:
    default:
      break;
  }
}

nlohmann::json SerializeProperties(const std::map<PropertyId, PropertyValue> &properties,
                                   NameIdMapper *name_id_mapper) {
  nlohmann::json serialized_properties = nlohmann::json::value_t::object;
  for (const auto &[prop_id, prop_value] : properties) {
    SerializeOne(serialized_properties, name_id_mapper, prop_id, prop_value);
  }
  return serialized_properties;
}

nlohmann::json SerializeProperties(std::span<PropertyPath const> paths, std::span<PropertyValue const> values,
                                   NameIdMapper *name_id_mapper) {
  nlohmann::json serialized_properties = nlohmann::json::value_t::object;
  for (auto const &[path, prop_value] : rv::zip(paths, values)) {
    SerializeOne(serialized_properties, name_id_mapper, path.back(), prop_value);
  }
  return serialized_properties;
}

std::string StringifyProperties(const std::map<PropertyId, PropertyValue> &properties) {
  std::vector<std::string> indexable_properties_as_string;
  for (const auto &[_, prop_value] : properties) StringifyOne(indexable_properties_as_string, prop_value);
  return utils::Join(indexable_properties_as_string, " ");
}

std::string StringifyProperties(std::span<PropertyValue const> values) {
  std::vector<std::string> indexable_properties_as_string;
  indexable_properties_as_string.reserve(values.size());
  for (auto const &prop_value : values) StringifyOne(indexable_properties_as_string, prop_value);
  return utils::Join(indexable_properties_as_string, " ");
}

}  // namespace

TextIndexFields TextFieldsOf(const std::map<PropertyId, PropertyValue> &properties, NameIdMapper *name_id_mapper) {
  return {.properties = SerializeProperties(properties, name_id_mapper),
          .all_property_values = StringifyProperties(properties)};
}

TextIndexFields TextFieldsOf(std::span<PropertyPath const> paths, std::span<PropertyValue const> values,
                             NameIdMapper *name_id_mapper) {
  return {.properties = SerializeProperties(paths, values, name_id_mapper),
          .all_property_values = StringifyProperties(values)};
}

void TrackTextIndexChange(TextIndexChangeCollector &collector, std::span<TextIndexData *> indices, const Vertex *vertex,
                          TextIndexOp op) {
  if (!vertex) return;
  for (auto *idx : indices) {
    auto &entry = collector[idx];
    if (op == TextIndexOp::ADD) {
      // Retain any prior pending removal in this transaction so removing and restoring an indexed
      // label deletes the committed document before inserting the replacement document.
      entry.to_add.insert(vertex);
    } else if (op == TextIndexOp::UPDATE) {
      // On update we have to firstly remove the vertex from index and then add it back
      entry.to_remove.insert(vertex);
      entry.to_add.insert(vertex);
    } else {  // REMOVE
      entry.to_add.erase(vertex);
      entry.to_remove.insert(vertex);
    }
  }
}

void TrackTextEdgeIndexChange(TextEdgeIndexChangeCollector &collector, std::span<TextEdgeIndexData *> indices,
                              const Edge *edge, const Vertex *from_vertex, const Vertex *to_vertex, TextIndexOp op) {
  if (!edge || !from_vertex || !to_vertex) return;
  const EdgeWithVertices edge_with_vertices(edge, from_vertex, to_vertex);
  for (auto *idx : indices) {
    auto &entry = collector[idx];
    if (op == TextIndexOp::ADD) {
      entry.to_add.insert(edge_with_vertices);
    } else if (op == TextIndexOp::UPDATE) {
      // On update we have to firstly remove the edge from index and then add it back
      entry.to_remove.insert(edge);
      entry.to_add.insert(edge_with_vertices);
    } else {  // REMOVE
      entry.to_add.erase(edge_with_vertices);
      entry.to_remove.insert(edge);
    }
  }
}

bool IndexPropertiesMatch(std::span<const PropertyId> index_properties,
                          std::span<const PropertyId> properties_to_check) {
  if (index_properties.empty()) {
    return true;
  }
  return r::any_of(properties_to_check, [&](auto property_id) { return r::contains(index_properties, property_id); });
}

struct TextSearchSession::Impl {
  boost::unordered_flat_map<TextIndexKey, rust::Box<mgcxx::text_search::SearcherContext>, absl::Hash<TextIndexKey>>
      searchers;
};

TextSearchSession::TextSearchSession() : impl_(std::make_unique<Impl>()) {}

TextSearchSession::~TextSearchSession() = default;
TextSearchSession::TextSearchSession(TextSearchSession &&) noexcept = default;
TextSearchSession &TextSearchSession::operator=(TextSearchSession &&) noexcept = default;

mgcxx::text_search::SearcherContext *TextSearchSession::GetOrAcquire(TextIndexKey index_key,
                                                                     mgcxx::text_search::Context &ctx) {
  auto it = impl_->searchers.find(index_key);
  if (it != impl_->searchers.end()) return &*it->second;
  auto [inserted, _] = impl_->searchers.emplace(index_key, mgcxx::text_search::acquire_searcher(ctx));
  return &*inserted->second;
}

}  // namespace memgraph::storage
