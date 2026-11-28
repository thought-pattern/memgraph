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

#include "storage/v2/indices/point_index.hpp"

#include <boost/geometry.hpp>
#include <boost/geometry/index/predicates.hpp>

#include <cmath>
#include <utility>
#include "storage/v2/indices/active_indices_updater.hpp"
#include "storage/v2/indices/point_index_change_collector.hpp"
#include "storage/v2/indices/point_index_expensive_header.hpp"
#include "storage/v2/indices/point_iterator.hpp"
#include "storage/v2/property_value.hpp"
#include "storage/v2/vertex.hpp"
#include "utils/logging.hpp"

namespace memgraph::storage {

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

struct PointIndex {
  PointIndex() = default;

  PointIndex(std::span<Entry<IndexPointWGS2d>> points2dWGS, std::span<Entry<IndexPointCartesian2d>> points2dCartesian,
             std::span<Entry<IndexPointWGS3d>> points3dWGS, std::span<Entry<IndexPointCartesian3d>> points3dCartesian);

  auto CreateNewPointIndex(LabelPropKey labelPropKey, ChangedVertices const &changed_vertices) const -> PointIndex;

  auto EntryCount() const -> std::size_t {
    return wgs_2d_index_->size() + wgs_3d_index_->size() + cartesian_2d_index_->size() + cartesian_3d_index_->size();
  }

  auto GetWgs2dIndex() const -> std::shared_ptr<index_t<IndexPointWGS2d>> { return wgs_2d_index_; }

  auto GetWgs3dIndex() const -> std::shared_ptr<index_t<IndexPointWGS3d>> { return wgs_3d_index_; }

  auto GetCartesian2dIndex() const -> std::shared_ptr<index_t<IndexPointCartesian2d>> { return cartesian_2d_index_; }

  auto GetCartesian3dIndex() const -> std::shared_ptr<index_t<IndexPointCartesian3d>> { return cartesian_3d_index_; }

 private:
  PointIndex(std::shared_ptr<index_t<IndexPointWGS2d>> points2dWGS,
             std::shared_ptr<index_t<IndexPointWGS3d>> points3dWGS,
             std::shared_ptr<index_t<IndexPointCartesian2d>> points2dCartesian,
             std::shared_ptr<index_t<IndexPointCartesian3d>> points3dCartesian);

  std::shared_ptr<index_t<IndexPointWGS2d>> wgs_2d_index_ = std::make_shared<index_t<IndexPointWGS2d>>();
  std::shared_ptr<index_t<IndexPointWGS3d>> wgs_3d_index_ = std::make_shared<index_t<IndexPointWGS3d>>();
  std::shared_ptr<index_t<IndexPointCartesian2d>> cartesian_2d_index_ =
      std::make_shared<index_t<IndexPointCartesian2d>>();
  std::shared_ptr<index_t<IndexPointCartesian3d>> cartesian_3d_index_ =
      std::make_shared<index_t<IndexPointCartesian3d>>();
};

namespace {
auto update_internal(index_container_t const &src, TrackedChanges const &tracked_changes)
    -> std::optional<index_container_t> {
  // All previous txns will use older index, this new built index will not concurrently be seen by older txns
  if (!tracked_changes.AnyChanges()) return std::nullopt;

  auto new_point_index = index_container_t{};

  for (auto const &key : src | std::views::keys) {
    auto tracked_changes_it = tracked_changes.find(key);

    auto const &key_src_index = std::invoke([&]() -> PointIndex const & {
      auto it = src.find(key);
      if (it != src.end()) return *it->second;
      static auto const empty_index = PointIndex{};
      return empty_index;
    });

    DMG_ASSERT(tracked_changes_it != tracked_changes.end(),
               "We expect the set of label+properties we are tracking to not have changed");

    auto const &changed_vertices = tracked_changes_it->second;
    if (changed_vertices.empty()) {
      // unmodified, no changes, just copy as is
      auto it = src.find(key);
      DMG_ASSERT(it != src.end());
      new_point_index.emplace(key, it->second);
    } else {
      // this key has changes, need to rebuild PointIndex for it
      new_point_index.emplace(key,
                              std::make_shared<PointIndex>(key_src_index.CreateNewPointIndex(key, changed_vertices)));
    }
  }

  return new_point_index;
}
}  // namespace

// ---- PointIndexStorage::ActiveIndices (snapshot) methods ----

bool PointIndexStorage::ActiveIndices::PointIndexExists(LabelId labelId, PropertyId propertyId) const {
  return indexes_->contains(LabelPropKey{labelId, propertyId});
}

std::optional<uint64_t> PointIndexStorage::ActiveIndices::ApproximatePointCount(LabelId labelId,
                                                                                PropertyId propertyId) const {
  auto it = indexes_->find(LabelPropKey{labelId, propertyId});
  if (it == indexes_->end()) return std::nullopt;
  return it->second->EntryCount();
}

std::vector<std::pair<LabelId, PropertyId>> PointIndexStorage::ActiveIndices::ListIndices() const {
  auto keys = *indexes_ | std::views::keys |
              std::views::transform([](LabelPropKey key) { return std::pair{key.label(), key.property()}; });
  return {keys.begin(), keys.end()};
}

// ---- PointIndexStorage (owner) methods ----

void PointIndexStorage::PublishActiveIndices(ActiveIndicesUpdater const &updater) {
  updater(std::make_shared<PointIndexStorage::ActiveIndices>(LoadIndexes()));
}

bool PointIndexStorage::CreatePointIndex(LabelId label, PropertyId property,
                                         utils::SkipListDb<Vertex>::Accessor vertices,
                                         ProgressCallback const &on_progress) {
  auto key = LabelPropKey{label, property};
  if (LoadIndexes()->contains(key)) return false;

  auto points_2d_WGS = std::vector<Entry<IndexPointWGS2d>>{};
  auto points_2d_Crt = std::vector<Entry<IndexPointCartesian2d>>{};
  auto points_3d_WGS = std::vector<Entry<IndexPointWGS3d>>{};
  auto points_3d_Crt = std::vector<Entry<IndexPointCartesian3d>>{};

  for (auto const &v : vertices) {
    if (on_progress) on_progress();
    if (v.deleted()) continue;
    if (!std::ranges::contains(v.labels, label)) continue;

    static constexpr auto point_types = std::array{PropertyStoreType::POINT};
    auto maybe_value = v.properties.GetPropertyOfTypes(property, point_types);
    if (!maybe_value) continue;

    auto value = *maybe_value;
    switch (value.type()) {
      case PropertyValueType::Point2d: {
        auto val = value.ValuePoint2d();
        if (IsWGS(val.crs())) {
          points_2d_WGS.emplace_back(IndexPointWGS2d{val}, &v);
        } else {
          points_2d_Crt.emplace_back(IndexPointCartesian2d{val}, &v);
        }
        break;
      }
      case PropertyValueType::Point3d: {
        auto val = value.ValuePoint3d();
        if (IsWGS(val.crs())) {
          points_3d_WGS.emplace_back(IndexPointWGS3d{val}, &v);
        } else {
          points_3d_Crt.emplace_back(IndexPointCartesian3d{val}, &v);
        }
        break;
      }
      default:
        continue;
    }
  }
  auto cur_indexes = LoadIndexes();
  if (cur_indexes->contains(key)) return false;
  auto new_index = std::make_shared<PointIndex>(points_2d_WGS, points_2d_Crt, points_3d_WGS, points_3d_Crt);
  auto new_indexes = std::make_shared<index_container_t>(*cur_indexes);
  new_indexes->try_emplace(key, std::move(new_index));
  StoreIndexes(std::move(new_indexes));
  return true;
}

void PointIndexStorage::RestorePointIndex(LabelId label, PropertyId property, std::shared_ptr<PointIndex const> index) {
  if (!index) return;
  auto key = LabelPropKey{label, property};
  auto cur_indexes = LoadIndexes();
  if (cur_indexes->contains(key)) return;
  auto new_indexes = std::make_shared<index_container_t>(*cur_indexes);
  new_indexes->emplace(key, std::move(index));
  StoreIndexes(std::move(new_indexes));
}

std::shared_ptr<PointIndex const> PointIndexStorage::DropPointIndex(LabelId label, PropertyId property) {
  auto key = LabelPropKey{label, property};
  auto cur_indexes = LoadIndexes();
  auto it = cur_indexes->find(key);
  if (it == cur_indexes->end()) return nullptr;

  auto evicted = it->second;
  auto new_indexes = std::make_shared<index_container_t>(*cur_indexes);
  new_indexes->erase(key);
  StoreIndexes(std::move(new_indexes));
  return evicted;
}

void PointIndexStorage::InstallNewPointIndex(PointIndexChangeCollector &collector, PointIndexContext &context,
                                             ActiveIndicesUpdater const &updater) {
  if (!context.UsingLocalIndex() && !collector.CurrentChanges().AnyChanges()) {
    return;
  }

  auto cur_indexes = LoadIndexes();
  auto noOtherIndexUpdate = cur_indexes == context.orig_indexes_;
  if (noOtherIndexUpdate) {
    context.update_current(collector);
    StoreIndexes(context.current_indexes_);
  } else {
    context.rebuild_current(cur_indexes, collector);
    StoreIndexes(context.current_indexes_);
  };

  PublishActiveIndices(updater);
}

void PointIndexStorage::Clear() { StoreIndexes(std::make_shared<index_container_t>()); }

std::vector<std::pair<LabelId, PropertyId>> PointIndexStorage::ListIndices() {
  auto indexes = LoadIndexes();
  auto keys = *indexes | std::views::keys |
              std::views::transform([](LabelPropKey key) { return std::pair{key.label(), key.property()}; });
  return {keys.begin(), keys.end()};
}

bool PointIndexStorage::PointIndexExists(LabelId labelId, PropertyId propertyId) {
  auto indexes = LoadIndexes();
  return indexes->contains(LabelPropKey{labelId, propertyId});
}

auto PointIndex::CreateNewPointIndex(LabelPropKey labelPropKey, ChangedVertices const &changed_vertices) const
    -> PointIndex {
  DMG_ASSERT(!changed_vertices.empty(), "Expect at least one change");

  // These are at this stage uncommitted changes + must have come from this txn, no need to check MVCC history
  auto changed_wgs_2d = std::unordered_map<Vertex const *, IndexPointWGS2d>{};
  auto changed_wgs_3d = std::unordered_map<Vertex const *, IndexPointWGS3d>{};
  auto changed_cartesian_2d = std::unordered_map<Vertex const *, IndexPointCartesian2d>{};
  auto changed_cartesian_3d = std::unordered_map<Vertex const *, IndexPointCartesian3d>{};

  // Single pass over all changes to cache current values
  for (auto const *v : changed_vertices) {
    auto guard = std::shared_lock{v->lock};
    auto isDeleted = [](Vertex const *v) { return v->deleted(); };
    auto isWithoutLabel = [label = labelPropKey.label()](Vertex const *v) {
      return !std::ranges::contains(v->labels, label);
    };
    if (isDeleted(v) || isWithoutLabel(v)) {
      continue;
    }
    constexpr auto all_point_types = std::array{PropertyStoreType::POINT};
    auto prop = v->properties.GetPropertyOfTypes(labelPropKey.property(), all_point_types);
    if (!prop) continue;
    if (prop->IsPoint2d()) {
      auto const &val = prop->ValuePoint2d();
      if (IsWGS(val.crs())) {
        changed_wgs_2d.emplace(v, val);
      } else {
        changed_cartesian_2d.emplace(v, val);
      }

    } else {
      auto const &val = prop->ValuePoint3d();
      if (IsWGS(val.crs())) {
        changed_wgs_3d.emplace(v, val);
      } else {
        changed_cartesian_3d.emplace(v, val);
      }
    }
  }

  auto helper = [&]<typename PointType>(std::shared_ptr<index_t<PointType>> const &index,
                                        std::unordered_map<Vertex const *, PointType> const &changed_values

                                        ) -> std::shared_ptr<index_t<PointType>> {
    auto modified = [&](Entry<PointType> const &entry) { return changed_vertices.contains(entry.vertex()); };
    if (changed_values.empty() && !std::ranges::any_of(*index, modified)) {
      // was unmodified, no need to rebuild
      return index;
    }

    auto copy_old = *index | std::views::filter(std::not_fn(modified));
    auto as_entry = [](std::pair<Vertex const *, PointType> const &p) { return Entry<PointType>{p.second, p.first}; };
    auto insert_modified = changed_values | std::views::transform(as_entry);

    auto new_index = std::make_shared<index_t<PointType>>(copy_old.begin(), copy_old.end());
    new_index->insert(insert_modified.begin(), insert_modified.end());
    return new_index;
  };

  return PointIndex{helper(wgs_2d_index_, changed_wgs_2d),
                    helper(wgs_3d_index_, changed_wgs_3d),
                    helper(cartesian_2d_index_, changed_cartesian_2d),
                    helper(cartesian_3d_index_, changed_cartesian_3d)};
}

void PointIndexContext::rebuild_current(std::shared_ptr<index_container_t> latest_index,
                                        PointIndexChangeCollector &collector) {
  orig_indexes_ = std::move(latest_index);
  current_indexes_ = orig_indexes_;

  auto result = update_internal(*current_indexes_, collector.PreviousChanges());
  if (result) current_indexes_ = std::make_unique<index_container_t>(*std::move(result));

  update_current(collector);
}

void PointIndexContext::update_current(PointIndexChangeCollector &collector) {
  auto result = update_internal(*current_indexes_, collector.CurrentChanges());
  if (result) current_indexes_ = std::make_unique<index_container_t>(*std::move(result));
  collector.ArchiveCurrentChanges();
}

auto PointIndexContext::PointVertices(LabelId label, PropertyId property, CoordinateReferenceSystem crs,
                                      Storage *storage, Transaction *transaction, PropertyValue const &point_value,
                                      PropertyValue const &boundary_value, PointDistanceCondition condition)
    -> PointIterable {
  auto const &indexes = *current_indexes_;
  auto it = indexes.find(LabelPropKey{label, property});
  if (it == indexes.cend()) {
    spdlog::warn("Failure: trying to locate a point index that should exist");
    return {};
  }

  auto const &point_index = *it->second;
  return {storage, transaction, point_index, crs, point_value, boundary_value, condition};
}

auto PointIndexContext::PointVertices(LabelId label, PropertyId property, CoordinateReferenceSystem crs,
                                      Storage *storage, Transaction *transaction, PropertyValue const &bottom_left,
                                      PropertyValue const &top_right, WithinBBoxCondition condition) -> PointIterable {
  auto const &indexes = *current_indexes_;
  auto it = indexes.find(LabelPropKey{label, property});
  if (it == indexes.cend()) {
    spdlog::warn("Failure: trying to locate a point index that should exist");
    return {};
  }

  auto const &point_index = *it->second;
  return {storage, transaction, point_index, crs, bottom_left, top_right, condition};
}

PointIndex::PointIndex(std::span<Entry<IndexPointWGS2d>> points2dWGS,
                       std::span<Entry<IndexPointCartesian2d>> points2dCartesian,
                       std::span<Entry<IndexPointWGS3d>> points3dWGS,
                       std::span<Entry<IndexPointCartesian3d>> points3dCartesian)
    : wgs_2d_index_{std::make_shared<index_t<IndexPointWGS2d>>(points2dWGS.begin(), points2dWGS.end())},
      wgs_3d_index_{std::make_shared<index_t<IndexPointWGS3d>>(points3dWGS.begin(), points3dWGS.end())},
      cartesian_2d_index_{
          std::make_shared<index_t<IndexPointCartesian2d>>(points2dCartesian.begin(), points2dCartesian.end())},
      cartesian_3d_index_{
          std::make_shared<index_t<IndexPointCartesian3d>>(points3dCartesian.begin(), points3dCartesian.end())} {}

PointIndex::PointIndex(std::shared_ptr<index_t<IndexPointWGS2d>> points2dWGS,
                       std::shared_ptr<index_t<IndexPointWGS3d>> points3dWGS,
                       std::shared_ptr<index_t<IndexPointCartesian2d>> points2dCartesian,
                       std::shared_ptr<index_t<IndexPointCartesian3d>> points3dCartesian)
    : wgs_2d_index_{std::move(points2dWGS)},
      wgs_3d_index_{std::move(points3dWGS)},
      cartesian_2d_index_{std::move(points2dCartesian)},
      cartesian_3d_index_{std::move(points3dCartesian)} {}

struct PointIterable::impl {
  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointWGS2d>> index,
                PropertyValue point_value, PropertyValue boundary_value, PointDistanceCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::WGS84_2d},
        using_distance_(true),
        point_value_{std::move(point_value)},
        boundary_value_{std::move(boundary_value)},
        distance_condition_{condition},
        wgs84_2d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointWGS3d>> index,
                PropertyValue point_value, PropertyValue boundary_value, PointDistanceCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::WGS84_3d},
        using_distance_(true),
        point_value_{std::move(point_value)},
        boundary_value_{std::move(boundary_value)},
        distance_condition_{condition},
        wgs84_3d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointCartesian2d>> index,
                PropertyValue point_value, PropertyValue boundary_value, PointDistanceCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::Cartesian_2d},
        using_distance_(true),
        point_value_{std::move(point_value)},
        boundary_value_{std::move(boundary_value)},
        distance_condition_{condition},
        cartesian_2d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointCartesian3d>> index,
                PropertyValue point_value, PropertyValue boundary_value, PointDistanceCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::Cartesian_3d},
        using_distance_(true),
        point_value_{std::move(point_value)},
        boundary_value_{std::move(boundary_value)},
        distance_condition_{condition},
        cartesian_3d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointWGS2d>> index,
                PropertyValue bottom_left, PropertyValue top_right, WithinBBoxCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::WGS84_2d},
        using_distance_(false),
        bottom_left_{std::move(bottom_left)},
        top_right_{std::move(top_right)},
        withinbbox_condition_{condition},
        wgs84_2d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointWGS3d>> index,
                PropertyValue bottom_left, PropertyValue top_right, WithinBBoxCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::WGS84_3d},
        using_distance_(false),
        bottom_left_{std::move(bottom_left)},
        top_right_{std::move(top_right)},
        withinbbox_condition_{condition},
        wgs84_3d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointCartesian2d>> index,
                PropertyValue bottom_left, PropertyValue top_right, WithinBBoxCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::Cartesian_2d},
        using_distance_(false),
        bottom_left_{std::move(bottom_left)},
        top_right_{std::move(top_right)},
        withinbbox_condition_{condition},
        cartesian_2d_{std::move(index)} {}

  explicit impl(Storage *storage, Transaction *transaction, std::shared_ptr<index_t<IndexPointCartesian3d>> index,
                PropertyValue bottom_left, PropertyValue top_right, WithinBBoxCondition condition)
      : storage_{storage},
        transaction_{transaction},
        crs_{CoordinateReferenceSystem::Cartesian_3d},
        using_distance_(false),
        bottom_left_{std::move(bottom_left)},
        top_right_{std::move(top_right)},
        withinbbox_condition_{condition},
        cartesian_3d_{std::move(index)} {}

  friend struct PointIterable;

  impl(impl const &) = delete;
  impl(impl &&) = delete;
  impl &operator=(impl const &) = delete;
  impl &operator=(impl &&) = delete;

  ~impl() {
    switch (crs_) {
      case CoordinateReferenceSystem::WGS84_2d:
        std::destroy_at(&wgs84_2d_);
        break;
      case CoordinateReferenceSystem::WGS84_3d:
        std::destroy_at(&wgs84_3d_);
        break;
      case CoordinateReferenceSystem::Cartesian_2d:
        std::destroy_at(&cartesian_2d_);
        break;
      case CoordinateReferenceSystem::Cartesian_3d:
        std::destroy_at(&cartesian_3d_);
        break;
    }
    if (using_distance_) {
      std::destroy_at(&point_value_);
    } else {
      std::destroy_at(&bottom_left_);
      std::destroy_at(&top_right_);
    }
  }

 private:
  Storage *storage_;
  Transaction *transaction_;
  CoordinateReferenceSystem crs_;
  bool using_distance_;

  union {
    struct {
      PropertyValue point_value_;
      PropertyValue boundary_value_;
      PointDistanceCondition distance_condition_;
    };

    struct {
      PropertyValue bottom_left_;
      PropertyValue top_right_;
      WithinBBoxCondition withinbbox_condition_;
    };
  };

  union {
    std::shared_ptr<index_t<IndexPointWGS2d>> wgs84_2d_;
    std::shared_ptr<index_t<IndexPointWGS3d>> wgs84_3d_;
    std::shared_ptr<index_t<IndexPointCartesian2d>> cartesian_2d_;
    std::shared_ptr<index_t<IndexPointCartesian3d>> cartesian_3d_;
  };
};

PointIterable::PointIterable() : pimpl{nullptr} {};
PointIterable::~PointIterable() = default;
PointIterable::PointIterable(PointIterable &&) noexcept = default;
PointIterable &PointIterable::operator=(PointIterable &&) = default;

namespace {
auto make_pimpl(PointIndex const &index, storage::CoordinateReferenceSystem crs, auto &&func) {
  switch (crs) {
    case CoordinateReferenceSystem::WGS84_2d: {
      return func(index.GetWgs2dIndex());
    }
    case CoordinateReferenceSystem::WGS84_3d: {
      return func(index.GetWgs3dIndex());
    }
    case CoordinateReferenceSystem::Cartesian_2d: {
      return func(index.GetCartesian2dIndex());
    }
    case CoordinateReferenceSystem::Cartesian_3d: {
      return func(index.GetCartesian3dIndex());
    }
  }
};
}  // namespace

PointIterable::PointIterable(Storage *storage, Transaction *transaction, PointIndex const &index,
                             storage::CoordinateReferenceSystem crs, PropertyValue const &point_value,
                             PropertyValue const &boundary_value, PointDistanceCondition condition)
    : pimpl{make_pimpl(index, crs, [&](auto specific_index) {
        return std::make_unique<impl>(
            storage, transaction, std::move(specific_index), point_value, boundary_value, condition);
      })} {}

PointIterable::PointIterable(Storage *storage, Transaction *transaction, PointIndex const &index,
                             storage::CoordinateReferenceSystem crs, PropertyValue const &bottom_left,
                             PropertyValue const &top_right, WithinBBoxCondition condition)
    : pimpl{make_pimpl(index, crs, [&](auto specific_index) {
        return std::make_unique<impl>(
            storage, transaction, std::move(specific_index), bottom_left, top_right, condition);
      })} {}

namespace {

double toRadians(double degrees) { return degrees * M_PI / 180.0; }

double toDegrees(double radians) { return radians * 180.0 / M_PI; }

template <typename point_type>
  requires std::is_same_v<typename bg::traits::coordinate_system<point_type>::type, bg::cs::cartesian>
auto create_outer_bounding_box(const point_type &center_point, double boundary) -> bg::model::box<point_type> {
  constexpr auto n_dimensions = bg::traits::dimension<point_type>::value;
  return [&]<auto... I>(std::index_sequence<I...>) {
    auto const min_corner = point_type{(bg::get<I>(center_point) - boundary)...};
    auto const max_corner = point_type{(bg::get<I>(center_point) + boundary)...};
    return bg::model::box{min_corner, max_corner};
  }(std::make_index_sequence<n_dimensions>{});
}

template <typename point_type>
  requires std::is_same_v<typename bg::traits::coordinate_system<point_type>::type, bg::cs::cartesian>
auto create_inner_exclusion_box(const point_type &center_point, double boundary)
    -> std::optional<bg::model::box<point_type>> {
  constexpr auto n_dimensions = bg::traits::dimension<point_type>::value;
  auto const offset = boundary / std::sqrt(n_dimensions);
  auto const slightly_smaller = std::max(0.0, std::nexttoward(offset, 0.0));
  if (slightly_smaller <= 0.0) {
    return std::nullopt;
  }
  return create_outer_bounding_box(center_point, slightly_smaller);
}

template <typename point_type>
  requires std::is_same_v<typename bg::traits::coordinate_system<point_type>::type, bg::cs::geographic<bg::degree>>
auto create_outer_bounding_box(const point_type &center_point, double boundary) -> bg::model::box<point_type> {
  // Minimum meridional/normal radius of curvature on the WGS-84 ellipsoid (a*(1 - e^2) at the equator).
  // Every WGS-84 tangent vector is at least kWgs84MinCurvatureRadius * unit-sphere arc length, so dividing
  // by a conservative floor strictly below a*(1 - e^2) guarantees the box covers every point within `boundary`.
  constexpr double kWgs84MinCurvatureRadius = 6'335'439.0;
  double const radDist = boundary / kWgs84MinCurvatureRadius;

  auto const radLon = toRadians(bg::get<0>(center_point));
  auto const radLat = toRadians(bg::get<1>(center_point));

  constexpr auto MIN_LAT = -M_PI_2;
  constexpr auto MAX_LAT = M_PI_2;

  constexpr auto MIN_LON = -M_PI;
  constexpr auto MAX_LON = M_PI;

  double minLat = radLat - radDist;
  double maxLat = radLat + radDist;

  double minLon;  // NOLINT(cppcoreguidelines-init-variables)
  double maxLon;  // NOLINT(cppcoreguidelines-init-variables)
  if (radDist < M_PI_2 && minLat > MIN_LAT && maxLat < MAX_LAT) {
    double const ratio = std::sin(radDist) / std::cos(radLat);
    if (ratio < 1.0) {
      double const deltaLon = std::asin(ratio);
      minLon = radLon - deltaLon;
      if (minLon < MIN_LON) minLon += 2.0 * M_PI;
      maxLon = radLon + deltaLon;
      if (maxLon > MAX_LON) maxLon -= 2.0 * M_PI;
      if (maxLon < minLon) maxLon += 2.0 * M_PI;
    } else {
      minLon = MIN_LON;
      maxLon = MAX_LON;
    }
  } else {
    minLat = std::max(minLat, MIN_LAT);
    maxLat = std::min(maxLat, MAX_LAT);
    minLon = MIN_LON;
    maxLon = MAX_LON;
  }

  constexpr auto n_dimensions = bg::traits::dimension<point_type>::value;
  if constexpr (n_dimensions == 2) {
    auto min_corner = point_type{toDegrees(minLon), toDegrees(minLat)};
    auto max_corner = point_type{toDegrees(maxLon), toDegrees(maxLat)};
    return bg::model::box<point_type>{min_corner, max_corner};
  } else {
    auto height_center = bg::get<2>(center_point);
    auto min_corner = point_type{toDegrees(minLon), toDegrees(minLat), height_center - boundary};
    auto max_corner = point_type{toDegrees(maxLon), toDegrees(maxLat), height_center + boundary};
    return bg::model::box<point_type>{min_corner, max_corner};
  }
}

template <typename point_type>
  requires std::is_same_v<typename bg::traits::coordinate_system<point_type>::type, bg::cs::geographic<bg::degree>>
auto create_inner_exclusion_box(const point_type &center_point, double boundary)
    -> std::optional<bg::model::box<point_type>> {
  // Maximum meridional/normal radius of curvature on the WGS-84 ellipsoid (a^2/b at the poles).
  // Clamping the inscribed box strictly inside [-90, 90] and [-180, 180] and scaling by cos_max guarantees
  // every point covered by this box has WGS-84 distance strictly less than `boundary` even near the poles.
  constexpr double kWgs84MaxCurvatureRadius = 6'399'594.0;
  constexpr auto n_dimensions = bg::traits::dimension<point_type>::value;
  double const component_budget = (boundary / std::sqrt(n_dimensions)) * 0.999999;
  if (!(component_budget > 0.0)) {
    return std::nullopt;
  }

  auto const radLon = toRadians(bg::get<0>(center_point));
  auto const radLat = toRadians(bg::get<1>(center_point));
  double const lat_room = std::min(radLat - (-M_PI_2), M_PI_2 - radLat);
  double const lon_room = std::min(radLon - (-M_PI), M_PI - radLon);
  double const dLat = std::min(component_budget / kWgs84MaxCurvatureRadius, lat_room * 0.999999);
  if (!(dLat > 0.0) || !(lon_room > 0.0)) {
    return std::nullopt;
  }

  double const minLat = radLat - dLat;
  double const maxLat = radLat + dLat;
  double const cos_max = (minLat <= 0.0 && 0.0 <= maxLat) ? 1.0 : std::max(std::cos(minLat), std::cos(maxLat));
  if (!(cos_max > 1e-12)) {
    return std::nullopt;
  }
  double const dLon = std::min((component_budget / kWgs84MaxCurvatureRadius) / cos_max, lon_room * 0.999999);
  if (!(dLon > 0.0)) {
    return std::nullopt;
  }

  double const minLon = radLon - dLon;
  double const maxLon = radLon + dLon;
  if constexpr (n_dimensions == 2) {
    return bg::model::box<point_type>{point_type{toDegrees(minLon), toDegrees(minLat)},
                                      point_type{toDegrees(maxLon), toDegrees(maxLat)}};
  } else {
    auto const height_center = bg::get<2>(center_point);
    return bg::model::box<point_type>{
        point_type{toDegrees(minLon), toDegrees(minLat), height_center - component_budget},
        point_type{toDegrees(maxLon), toDegrees(maxLat), height_center + component_budget}};
  }
}

template <typename Index>
auto get_index_iterator_distance(Index const &index, PropertyValue const &point_value,
                                 PropertyValue const &boundary_value, PointDistanceCondition condition)
    -> Index::const_query_iterator {
  double boundary =
      boundary_value.IsInt() ? static_cast<double>(boundary_value.ValueInt()) : boundary_value.ValueDouble();

  using enum PointDistanceCondition;
  if (boundary < 0.0) {
    // point.distance() will always be positive, a negative boundary needs special handling
    if ((condition == INSIDE || condition == INSIDE_AND_BOUNDARY)) {
      //  < or <= will be always false
      return index.qend();
    }
    // > or >= will be always true
    return index.qbegin(bgi::satisfies([](auto const &) { return true; }));
  }

  using point_type = Index::value_type::point_type;
  auto constexpr dimensions = bg::traits::dimension<point_type>::value;
  using CoordinateSystem = typename bg::traits::coordinate_system<point_type>::type;
  auto constexpr is_cartesian = std::is_same_v<CoordinateSystem, bg::cs::cartesian>;

  auto center_point = std::invoke([&]() -> point_type {
    if constexpr (dimensions == 2) {
      auto tmp_point = point_value.ValuePoint2d();
      return {tmp_point.x(), tmp_point.y()};
    } else {
      auto tmp_point = point_value.ValuePoint3d();
      return {tmp_point.x(), tmp_point.y(), tmp_point.z()};
    }
  });

  auto outer_inclusion_box = [&] {
    auto slightly_larger = std::nexttoward(boundary, std::numeric_limits<long double>::infinity());
    return create_outer_bounding_box(center_point, slightly_larger);
  };

  auto true_distance = [](auto a, auto b) {
    if constexpr (is_cartesian || dimensions == 2) {
      return bg::distance(a, b);
    } else {
      auto h1 = bg::get<2>(a);
      auto h2 = bg::get<2>(b);
      auto middle = std::midpoint(h1, h2);
      auto avg_a = point_type{bg::get<0>(a), bg::get<1>(a), middle};
      auto avg_b = point_type{bg::get<0>(b), bg::get<1>(b), middle};
      auto distance_spherical = bg::distance(avg_a, avg_b);

      auto height_diff = h1 - h2;
      return std::hypot(height_diff, distance_spherical);
    }
  };

  switch (condition) {
    case PointDistanceCondition::OUTSIDE: {
      if (auto const inner_box = create_inner_exclusion_box(center_point, boundary); inner_box.has_value()) {
        return index.qbegin(!bgi::covered_by(*inner_box) && bgi::satisfies([=](const auto &value) {
          return true_distance(value.point(), center_point) > boundary;
        }));
      }
      return index.qbegin(bgi::satisfies([=](const auto &value) {
        return true_distance(value.point(), center_point) > boundary;
      }));
    }
    case PointDistanceCondition::INSIDE: {
      return index.qbegin(bgi::covered_by(outer_inclusion_box()) && bgi::satisfies([=](const auto &value) {
                            return true_distance(value.point(), center_point) < boundary;
                          }));
    }
    case PointDistanceCondition::INSIDE_AND_BOUNDARY: {
      return index.qbegin(bgi::covered_by(outer_inclusion_box()) && bgi::satisfies([=](const auto &value) {
                            return true_distance(value.point(), center_point) <= boundary;
                          }));
    }
    case PointDistanceCondition::OUTSIDE_AND_BOUNDARY: {
      if (auto const inner_box = create_inner_exclusion_box(center_point, boundary); inner_box.has_value()) {
        return index.qbegin(!bgi::covered_by(*inner_box) && bgi::satisfies([=](const auto &value) {
          return true_distance(value.point(), center_point) >= boundary;
        }));
      }
      return index.qbegin(bgi::satisfies([=](const auto &value) {
        return true_distance(value.point(), center_point) >= boundary;
      }));
    }
  }
}

template <typename Index>
auto get_index_iterator_withinbbox(Index &index, PropertyValue const &bottom_left, PropertyValue const &top_right,
                                   WithinBBoxCondition condition) -> Index::const_query_iterator {
  using point_type = Index::value_type::point_type;
  auto constexpr dimensions = bg::traits::dimension<point_type>::value;

  auto const lower_bound = std::invoke([&bottom_left]() -> point_type {
    if constexpr (dimensions == 2) {
      auto tmp_point = bottom_left.ValuePoint2d();
      return {tmp_point.x(), tmp_point.y()};
    } else {
      auto tmp_point = bottom_left.ValuePoint3d();
      return {tmp_point.x(), tmp_point.y(), tmp_point.z()};
    }
  });

  auto const upper_bound = std::invoke([&lower_bound, &top_right]() -> point_type {
    using CoordinateSystem = typename bg::traits::coordinate_system<point_type>::type;
    auto constexpr is_cartesian = std::is_same_v<CoordinateSystem, bg::cs::cartesian>;
    if constexpr (dimensions == 2) {
      auto const tmp_point = top_right.ValuePoint2d();
      if constexpr (is_cartesian) return {tmp_point.x(), tmp_point.y()};

      auto const longitude = tmp_point.x();
      return {bg::get<0>(lower_bound) <= longitude ? longitude : longitude + 360.0, tmp_point.y()};
    } else {
      auto const tmp_point = top_right.ValuePoint3d();
      if constexpr (is_cartesian) return {tmp_point.x(), tmp_point.y(), tmp_point.z()};

      auto const longitude = tmp_point.x();
      return {bg::get<0>(lower_bound) <= longitude ? longitude : longitude + 360.0, tmp_point.y(), tmp_point.z()};
    }
  });

  auto const bounding_box = bg::model::box(lower_bound, upper_bound);

  switch (condition) {
    case WithinBBoxCondition::OUTSIDE: {
      return index.qbegin(!bgi::covered_by(bounding_box));
    }
    case WithinBBoxCondition::INSIDE: {
      return index.qbegin(bgi::covered_by(bounding_box));
    }
  }
}

}  // namespace

auto PointIterable::begin() const -> PointIterator {
  if (!pimpl) {
    static const index_t<IndexPointCartesian2d> empty_index{};
    return PointIterator{nullptr, nullptr, CoordinateReferenceSystem::Cartesian_2d, empty_index.qend()};
  }
  auto apply_index = [&](CoordinateReferenceSystem crs, auto &&func) {
    switch (crs) {
      case CoordinateReferenceSystem::WGS84_2d:
        return func(*pimpl->wgs84_2d_);
      case CoordinateReferenceSystem::WGS84_3d:
        return func(*pimpl->wgs84_3d_);
      case CoordinateReferenceSystem::Cartesian_2d:
        return func(*pimpl->cartesian_2d_);
      case CoordinateReferenceSystem::Cartesian_3d:
        return func(*pimpl->cartesian_3d_);
    }
  };

  if (pimpl->using_distance_) {
    auto make_distance_iter = [&](auto const &index) {
      return PointIterator{
          pimpl->storage_,
          pimpl->transaction_,
          pimpl->crs_,
          get_index_iterator_distance(index, pimpl->point_value_, pimpl->boundary_value_, pimpl->distance_condition_)};
    };
    return apply_index(pimpl->crs_, make_distance_iter);
  }

  auto make_withinbbox_iter = [&](auto const &index) {
    return PointIterator{
        pimpl->storage_,
        pimpl->transaction_,
        pimpl->crs_,
        get_index_iterator_withinbbox(index, pimpl->bottom_left_, pimpl->top_right_, pimpl->withinbbox_condition_)};
  };
  return apply_index(pimpl->crs_, make_withinbbox_iter);
}

auto PointIterable::end() const -> PointIterator {
  if (!pimpl) {
    static const index_t<IndexPointCartesian2d> empty_index{};
    return PointIterator{nullptr, nullptr, CoordinateReferenceSystem::Cartesian_2d, empty_index.qend()};
  }
  switch (pimpl->crs_) {
    case CoordinateReferenceSystem::WGS84_2d:
      return PointIterator{pimpl->storage_, pimpl->transaction_, pimpl->crs_, pimpl->wgs84_2d_->qend()};
    case CoordinateReferenceSystem::WGS84_3d:
      return PointIterator{pimpl->storage_, pimpl->transaction_, pimpl->crs_, pimpl->wgs84_3d_->qend()};
    case CoordinateReferenceSystem::Cartesian_2d:
      return PointIterator{pimpl->storage_, pimpl->transaction_, pimpl->crs_, pimpl->cartesian_2d_->qend()};
    case CoordinateReferenceSystem::Cartesian_3d:
      return PointIterator{pimpl->storage_, pimpl->transaction_, pimpl->crs_, pimpl->cartesian_3d_->qend()};
  }
}

}  // namespace memgraph::storage
