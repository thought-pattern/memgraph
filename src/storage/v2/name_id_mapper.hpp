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

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "utils/logging.hpp"
#include "utils/skip_list.hpp"
#include "utils/spin_lock.hpp"

namespace memgraph::storage {

class NameIdMapper {
 private:
  struct MapNameToId {
    std::string name;
    uint64_t id;

    bool operator<(const MapNameToId &other) const { return name < other.name; }

    bool operator==(const MapNameToId &other) const { return name == other.name; }

    bool operator<(const std::string_view other) const { return name < other; }

    bool operator==(const std::string_view other) const { return name == other; }
  };

  struct MapIdToName {
    uint64_t id;
    std::string name;

    bool operator<(const MapIdToName &other) const { return id < other.id; }

    bool operator==(const MapIdToName &other) const { return id == other.id; }

    bool operator<(uint64_t other) const { return id < other; }

    bool operator==(uint64_t other) const { return id == other; }
  };

 public:
  explicit NameIdMapper() = default;

  NameIdMapper(const NameIdMapper &) = delete;
  NameIdMapper &operator=(const NameIdMapper &) = delete;
  NameIdMapper(NameIdMapper &&) = delete;
  NameIdMapper &operator=(NameIdMapper &&) = delete;

  virtual ~NameIdMapper() = default;

  /// @throw std::bad_alloc if unable to insert a new mapping
  virtual uint64_t NameToId(const std::string_view name) {
    {
      auto name_to_id_acc = name_to_id_.access();
      auto found = name_to_id_acc.find(name);
      if (found != name_to_id_acc.end()) {
        return found->id;
      }
    }

    std::lock_guard const guard{write_lock_};
    {
      auto name_to_id_acc = name_to_id_.access();
      auto found = name_to_id_acc.find(name);
      if (found != name_to_id_acc.end()) {
        return found->id;
      }
    }

    std::string rev_name{name};
    std::string fwd_name{name};
    const uint64_t new_id = counter_.load(std::memory_order_relaxed);
    id_to_name_.access().insert({new_id, std::move(rev_name)});
    try {
      name_to_id_.access().insert({std::move(fwd_name), new_id});
    } catch (...) {
      id_to_name_.access().remove(new_id);
      throw;
    }
    counter_.store(new_id + 1, std::memory_order_release);
    return new_id;
  }

  /// This method unlike NameToId does not insert the new property id if not found
  /// but just returns either std::nullopt or the value of the property id if it
  /// finds it.
  virtual std::optional<uint64_t> NameToIdIfExists(const std::string_view name) const {
    auto name_to_id_acc = name_to_id_.access();
    auto found = name_to_id_acc.find(name);
    if (found == name_to_id_acc.end()) {
      return std::nullopt;
    }
    return found->id;
  }

  // NOTE: Currently this function returns a `const std::string &` instead of a
  // `std::string` to avoid making unnecessary copies of the string.
  // Usually, this wouldn't be correct because the accessor to the
  // `utils::SkipList` is destroyed in this function and that removes the
  // guarantee that the reference to the value contained in the list will be
  // valid.
  // Currently, we never delete anything from the `utils::SkipList` so the
  // references will always be valid. If you change this class to remove unused
  // names, be sure to change the signature of this function.
  virtual const std::string &IdToName(uint64_t id) {
    auto maybe_name = MaybeIdToName(id);
    MG_ASSERT(maybe_name.has_value(), "Trying to get a name for an invalid ID!");
    return maybe_name.value();
  }

  // Resets the mapper to its initial empty state. Used when scrubbing a storage
  // back to empty (e.g. bringing up a broken database after recovery failure).
  // Must not be called while other threads hold references returned by IdToName.
  virtual void Clear() {
    std::lock_guard const guard{write_lock_};
    name_to_id_.clear();
    id_to_name_.clear();
    counter_.store(0, std::memory_order_release);
  }

 protected:
  std::optional<std::reference_wrapper<const std::string>> MaybeIdToName(uint64_t id) const {
    auto id_to_name_acc = id_to_name_.access();
    auto result = id_to_name_acc.find(id);
    if (result == id_to_name_acc.end()) {
      return std::nullopt;
    }
    return result->name;
  }

  mutable utils::SpinLock write_lock_{};
  std::atomic<uint64_t> counter_{0};
  utils::SkipListDb<MapNameToId> name_to_id_;
  utils::SkipListDb<MapIdToName> id_to_name_;
};
}  // namespace memgraph::storage
