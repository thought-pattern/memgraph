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

#include "storage/v2/enum.hpp"
#include "storage/v2/name_id_mapper.hpp"

#include <boost/unordered/unordered_flat_map.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/zip.hpp>
#include "strong_type/strong_type.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <map>
#include <ranges>
#include <string>

namespace memgraph::storage {

enum struct EnumStorageError : uint8_t { EnumExists, InvalidValue, UnknownEnumType, UnknownEnumValue, ParseError };

struct EnumStore {
  auto RegisterEnum(std::string_view type_str, std::span<std::string const> enum_value_strs) {
    return RegisterEnum(std::string{type_str}, std::vector(enum_value_strs.begin(), enum_value_strs.end()));
  }

  auto RegisterEnum(std::string type_str, std::vector<std::string> enum_value_strs)
      -> std::expected<EnumTypeId, EnumStorageError> {
    namespace rv = ranges::views;

    if (etype_lookup_.contains(type_str)) [[unlikely]] {
      return std::unexpected(EnumStorageError::EnumExists);
    }

    auto values_lookup = decltype(evalue_lookups_)::value_type{};
    values_lookup.reserve(enum_value_strs.size());
    for (auto const &[pos, value] : rv::enumerate(enum_value_strs)) {
      auto [it_values, inserted] = values_lookup.try_emplace(value, EnumValueId{pos});
      if (!inserted) [[unlikely]] {
        return std::unexpected(EnumStorageError::InvalidValue);
      }
    }

    auto const new_pos = etype_strs_.size();
    auto const new_id = EnumTypeId{new_pos};
    evalue_lookups_.reserve(new_pos + 1);
    etype_strs_.reserve(new_pos + 1);
    evalue_strs_.reserve(new_pos + 1);

    auto [it_type, inserted] = etype_lookup_.try_emplace(type_str, new_id);
    if (!inserted) [[unlikely]] {
      return std::unexpected(EnumStorageError::EnumExists);
    }
    try {
      evalue_lookups_.emplace_back(std::move(values_lookup));
      etype_strs_.emplace_back(std::move(type_str));
      evalue_strs_.emplace_back(std::move(enum_value_strs));
    } catch (...) {
      etype_lookup_.erase(it_type);
      if (etype_lookup_.size() < evalue_lookups_.size()) evalue_lookups_.pop_back();
      if (etype_lookup_.size() < etype_strs_.size()) etype_strs_.pop_back();
      throw;
    }

    DMG_ASSERT(etype_strs_.size() == evalue_strs_.size());
    DMG_ASSERT(etype_strs_.size() == etype_lookup_.size());
    DMG_ASSERT(etype_strs_.size() == evalue_lookups_.size());

    return {new_id};
  }

  auto UpdateValue(std::string_view e_type, std::string_view old_value, std::string_view new_value)
      -> std::expected<Enum, EnumStorageError> {
    auto do_update = [&](EnumTypeId etype) { return UpdateValue(etype, old_value, new_value); };
    return ToEnumType(e_type).and_then(do_update);
  }

  auto UpdateValue(EnumTypeId e_type, std::string_view old_value, std::string_view new_value)
      -> std::expected<Enum, EnumStorageError> {
    auto e_value = ToEnumValue(e_type, old_value);
    if (!e_value) return std::unexpected(e_value.error());

    if (old_value == new_value) return std::unexpected{EnumStorageError::InvalidValue};

    auto &lookup = evalue_lookups_[e_type.value_of()];
    auto &strings = evalue_strs_[e_type.value_of()];

    if (lookup.contains(new_value)) return std::unexpected{EnumStorageError::InvalidValue};

    std::string updated_display{new_value};
    auto [it_new, inserted] = lookup.try_emplace(new_value, *e_value);
    if (!inserted) return std::unexpected{EnumStorageError::InvalidValue};
    strings[e_value->value_of()].swap(updated_display);
    lookup.erase(updated_display);
    MG_ASSERT(strings.size() == lookup.size());
    return Enum{e_type, *e_value};
  }

  auto AddValue(std::string_view e_type, std::string_view new_value) -> std::expected<Enum, EnumStorageError> {
    auto do_add = [&](EnumTypeId etype) { return AddValue(etype, new_value); };
    return ToEnumType(e_type).and_then(do_add);
  }

  auto AddValue(EnumTypeId e_type, std::string_view new_value) -> std::expected<Enum, EnumStorageError> {
    namespace rv = ranges::views;

    if (evalue_lookups_.size() <= e_type.value_of()) return std::unexpected{EnumStorageError::UnknownEnumType};

    auto &lookup = evalue_lookups_[e_type.value_of()];
    auto &strings = evalue_strs_[e_type.value_of()];

    if (lookup.contains(new_value)) return std::unexpected{EnumStorageError::InvalidValue};

    auto e_value = EnumValueId{strings.size()};
    strings.emplace_back(new_value);
    try {
      lookup.try_emplace(new_value, e_value);
    } catch (...) {
      strings.pop_back();
      throw;
    }
    MG_ASSERT(strings.size() == lookup.size());
    return Enum{e_type, e_value};
  }

  void RollbackRegisterEnum(EnumTypeId e_type) noexcept {
    if (e_type.value_of() + 1 == etype_strs_.size()) {
      etype_lookup_.erase(etype_strs_.back());
      etype_strs_.pop_back();
      evalue_strs_.pop_back();
      evalue_lookups_.pop_back();
    }
  }

  void RollbackAddValue(Enum added) noexcept {
    auto const tid = added.type_id().value_of();
    auto const vid = added.value_id().value_of();
    if (tid < evalue_strs_.size() && vid + 1 == evalue_strs_[tid].size()) {
      evalue_lookups_[tid].erase(evalue_strs_[tid].back());
      evalue_strs_[tid].pop_back();
    }
  }

  void RollbackUpdateValue(Enum updated, std::string restored_old_value) noexcept {
    auto const tid = updated.type_id().value_of();
    auto const vid = updated.value_id().value_of();
    if (tid < evalue_strs_.size() && vid < evalue_strs_[tid].size()) {
      auto &lookup = evalue_lookups_[tid];
      auto &strings = evalue_strs_[tid];
      auto [_, inserted] = lookup.try_emplace(restored_old_value, updated.value_id());
      if (inserted) {
        strings[vid].swap(restored_old_value);
        lookup.erase(restored_old_value);
      }
    }
  }

  // used by recovery in the event of failure
  void clear() {
    etype_strs_.clear();
    evalue_strs_.clear();
    etype_lookup_.clear();
    evalue_lookups_.clear();
  }

  auto ToEnumType(std::string_view type_str) const -> std::expected<EnumTypeId, EnumStorageError> {
    auto it = etype_lookup_.find(type_str);
    if (it == etype_lookup_.cend()) return std::unexpected{EnumStorageError::UnknownEnumType};
    return it->second;
  }

  auto ToEnumValue(std::string_view type_str, std::string_view value_str) const
      -> std::expected<EnumValueId, EnumStorageError> {
    auto lookup_value = [&](EnumTypeId etype) { return ToEnumValue(etype, value_str); };
    return ToEnumType(type_str).and_then(lookup_value);
  }

  auto ToEnumValue(EnumTypeId e_type, std::string_view value_str) const
      -> std::expected<EnumValueId, EnumStorageError> {
    if (evalue_lookups_.size() <= e_type.value_of()) return std::unexpected{EnumStorageError::UnknownEnumType};
    auto const &evalue_lookup = evalue_lookups_[e_type.value_of()];
    auto it = evalue_lookup.find(value_str);
    if (it == evalue_lookup.cend()) return std::unexpected{EnumStorageError::UnknownEnumValue};
    return it->second;
  }

  auto ToEnum(std::string_view enum_str) const -> std::expected<Enum, EnumStorageError> {
    auto pos = enum_str.find("::");
    if (pos == std::string_view::npos) return std::unexpected{EnumStorageError::ParseError};
    auto etype = enum_str.substr(0, pos);
    auto evalue = enum_str.substr(pos + 2);
    return ToEnum(etype, evalue);
  }

  auto ToEnum(std::string_view type_str, std::string_view value_str) const -> std::expected<Enum, EnumStorageError> {
    auto e_type = ToEnumType(type_str);
    if (!e_type) return std::unexpected(e_type.error());
    auto e_value = ToEnumValue(*e_type, value_str);
    if (!e_value) return std::unexpected(e_value.error());
    return Enum{*e_type, *e_value};
  }

  auto ToTypeString(EnumTypeId id) const -> std::expected<std::string, EnumStorageError> {
    if (etype_strs_.size() <= id.value_of()) return std::unexpected{EnumStorageError::UnknownEnumType};
    return etype_strs_[id.value_of()];
  }

  auto ToValuesStrings(EnumTypeId e_type) const -> std::vector<std::string> const * {
    if (evalue_strs_.size() <= e_type.value_of()) return nullptr;
    return std::addressof(evalue_strs_[e_type.value_of()]);
  }

  auto ToValueString(EnumTypeId e_type, EnumValueId e_value) const -> std::expected<std::string, EnumStorageError> {
    if (evalue_strs_.size() <= e_type.value_of()) return std::unexpected{EnumStorageError::UnknownEnumType};
    auto const &values = evalue_strs_[e_type.value_of()];
    if (values.size() <= e_value.value_of()) return std::unexpected{EnumStorageError::UnknownEnumValue};
    return values[e_value.value_of()];
  }

  auto ToString(Enum val) const -> std::expected<std::string, EnumStorageError> {
    auto type_str = ToTypeString(val.type_id());
    if (!type_str) return std::unexpected(type_str.error());
    auto value_str = ToValueString(val.type_id(), val.value_id());
    if (!value_str) return std::unexpected(value_str.error());

    return std::format("{}::{}", *type_str, *value_str);
  }

  auto AllRegistered() const { return ranges::views::zip(etype_strs_, evalue_strs_); }

 private:
  std::vector<std::string> etype_strs_;
  std::vector<std::vector<std::string>> evalue_strs_;

  // Transparent so lookups by string_view do not build a std::string
  struct StringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
  };

  template <typename V>
  using StringMap = boost::unordered_flat_map<std::string, V, StringHash, std::equal_to<>>;

  StringMap<EnumTypeId> etype_lookup_;
  std::vector<StringMap<EnumValueId>> evalue_lookups_;
};

}  // namespace memgraph::storage
