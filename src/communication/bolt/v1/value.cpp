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

#include "communication/bolt/v1/value.hpp"

#include <limits>

#include "query/string_helpers.hpp"
#include "utils/algorithm.hpp"
#include "utils/string.hpp"
#include "utils/temporal.hpp"

namespace memgraph::communication::bolt {

#define DEF_GETTER_BY_VAL(type, value_type, field)   \
  value_type &Value::Value##type() {                 \
    if (type_ != Type::type) throw ValueException(); \
    return field;                                    \
  }                                                  \
  value_type Value::Value##type() const {            \
    if (type_ != Type::type) throw ValueException(); \
    return field;                                    \
  }

DEF_GETTER_BY_VAL(Bool, bool, bool_v)
DEF_GETTER_BY_VAL(Int, int64_t, int_v)
DEF_GETTER_BY_VAL(Double, double, double_v)

#undef DEF_GETTER_BY_VAL

#define DEF_GETTER_BY_REF(type, value_type, field)   \
  value_type &Value::Value##type() {                 \
    if (type_ != Type::type) throw ValueException(); \
    return field;                                    \
  }                                                  \
  const value_type &Value::Value##type() const {     \
    if (type_ != Type::type) throw ValueException(); \
    return field;                                    \
  }

DEF_GETTER_BY_REF(String, std::string, string_v)
DEF_GETTER_BY_REF(List, std::vector<Value>, list_v)
DEF_GETTER_BY_REF(Map, map_t, map_v)
DEF_GETTER_BY_REF(Vertex, Vertex, vertex_v)
DEF_GETTER_BY_REF(Edge, Edge, edge_v)
DEF_GETTER_BY_REF(UnboundedEdge, UnboundedEdge, unbounded_edge_v)
DEF_GETTER_BY_REF(Path, Path, path_v)
DEF_GETTER_BY_REF(Date, utils::Date, date_v)
DEF_GETTER_BY_REF(LocalTime, utils::LocalTime, local_time_v)
DEF_GETTER_BY_REF(LocalDateTime, utils::LocalDateTime, local_date_time_v)
DEF_GETTER_BY_REF(Duration, utils::Duration, duration_v)
DEF_GETTER_BY_REF(ZonedDateTime, utils::ZonedDateTime, zoned_date_time_v)
DEF_GETTER_BY_REF(Point2d, Point2d, point_2d_v);
DEF_GETTER_BY_REF(Point3d, Point3d, point_3d_v);

#undef DEF_GETTER_BY_REF

bool Path::HasValidStructure() const noexcept {
  if (vertices.empty()) return false;
  if ((indices.size() % 2U) != 0U) return false;
  for (size_t i = 0; i < indices.size(); i += 2) {
    const int64_t edge_ind = indices[i];
    const int64_t vertex_ind = indices[i + 1];
    if (edge_ind == 0 || edge_ind == std::numeric_limits<int64_t>::min()) return false;
    const uint64_t abs_edge =
        edge_ind > 0 ? static_cast<uint64_t>(edge_ind) : static_cast<uint64_t>(-(edge_ind + 1)) + 1ULL;
    if (abs_edge - 1ULL >= edges.size()) return false;
    if (vertex_ind < 0 || static_cast<uint64_t>(vertex_ind) >= vertices.size()) return false;
  }
  return true;
}

void Value::CopyConstructFrom(const Value &other) {
  type_ = Type::Null;
  switch (other.type_) {
    case Type::Null:
      break;
    case Type::Bool:
      this->bool_v = other.bool_v;
      break;
    case Type::Int:
      this->int_v = other.int_v;
      break;
    case Type::Double:
      this->double_v = other.double_v;
      break;
    case Type::String:
      new (&string_v) std::string(other.string_v);
      break;
    case Type::List:
      new (&list_v) std::vector<Value>(other.list_v);
      break;
    case Type::Map:
      new (&map_v) map_t(other.map_v);
      break;
    case Type::Vertex:
      new (&vertex_v) Vertex(other.vertex_v);
      break;
    case Type::Edge:
      new (&edge_v) Edge(other.edge_v);
      break;
    case Type::UnboundedEdge:
      new (&unbounded_edge_v) UnboundedEdge(other.unbounded_edge_v);
      break;
    case Type::Path:
      new (&path_v) Path(other.path_v);
      break;
    case Type::Date:
      new (&date_v) utils::Date(other.date_v);
      break;
    case Type::LocalTime:
      new (&local_time_v) utils::LocalTime(other.local_time_v);
      break;
    case Type::LocalDateTime:
      new (&local_date_time_v) utils::LocalDateTime(other.local_date_time_v);
      break;
    case Type::Duration:
      new (&duration_v) utils::Duration(other.duration_v);
      break;
    case Type::ZonedDateTime:
      new (&zoned_date_time_v) utils::ZonedDateTime(other.zoned_date_time_v);
      break;
    case Type::Point2d:
      new (&point_2d_v) Point2d(other.point_2d_v);
      break;
    case Type::Point3d:
      new (&point_3d_v) Point3d(other.point_3d_v);
      break;
  }
  type_ = other.type_;
}

void Value::MoveConstructFrom(Value &&other) noexcept {
  type_ = other.type_;
  switch (other.type_) {
    case Type::Null:
      break;
    case Type::Bool:
      this->bool_v = other.bool_v;
      break;
    case Type::Int:
      this->int_v = other.int_v;
      break;
    case Type::Double:
      this->double_v = other.double_v;
      break;
    case Type::String:
      new (&string_v) std::string(std::move(other.string_v));
      break;
    case Type::List:
      new (&list_v) std::vector<Value>(std::move(other.list_v));
      break;
    case Type::Map:
      new (&map_v) map_t(std::move(other.map_v));
      break;
    case Type::Vertex:
      new (&vertex_v) Vertex(std::move(other.vertex_v));
      break;
    case Type::Edge:
      new (&edge_v) Edge(std::move(other.edge_v));
      break;
    case Type::UnboundedEdge:
      new (&unbounded_edge_v) UnboundedEdge(std::move(other.unbounded_edge_v));
      break;
    case Type::Path:
      new (&path_v) Path(std::move(other.path_v));
      break;
    case Type::Date:
      new (&date_v) utils::Date(other.date_v);
      break;
    case Type::LocalTime:
      new (&local_time_v) utils::LocalTime(other.local_time_v);
      break;
    case Type::LocalDateTime:
      new (&local_date_time_v) utils::LocalDateTime(other.local_date_time_v);
      break;
    case Type::Duration:
      new (&duration_v) utils::Duration(other.duration_v);
      break;
    case Type::ZonedDateTime:
      new (&zoned_date_time_v) utils::ZonedDateTime(other.zoned_date_time_v);
      break;
    case Type::Point2d:
      new (&point_2d_v) Point2d(other.point_2d_v);
      break;
    case Type::Point3d:
      new (&point_3d_v) Point3d(other.point_3d_v);
      break;
  }
}

void Value::DestroyActiveMember() noexcept {
  const Type prev_type = type_;
  type_ = Type::Null;
  switch (prev_type) {
    case Type::Null:
    case Type::Bool:
    case Type::Int:
    case Type::Double:
      return;
    case Type::String: {
      using namespace std;
      string_v.~string();
      return;
    }
    case Type::List: {
      using namespace std;
      list_v.~vector<Value>();
      return;
    }
    case Type::Map: {
      using namespace std;
      map_v.~map_t();
      return;
    }
    case Type::Vertex:
      vertex_v.~Vertex();
      return;
    case Type::Edge:
      edge_v.~Edge();
      return;
    case Type::UnboundedEdge:
      unbounded_edge_v.~UnboundedEdge();
      return;
    case Type::Path:
      path_v.~Path();
      return;
    case Type::Date:
      date_v.~Date();
      return;
    case Type::LocalTime:
      local_time_v.~LocalTime();
      return;
    case Type::LocalDateTime:
      local_date_time_v.~LocalDateTime();
      return;
    case Type::Duration:
      duration_v.~Duration();
      return;
    case Type::ZonedDateTime:
      zoned_date_time_v.~ZonedDateTime();
      return;
    case Type::Point2d:
      point_2d_v.~Point2d();
      return;
    case Type::Point3d:
      point_3d_v.~Point3d();
      return;
  }
}

Value::Value(const Value &other) : type_(Type::Null) { CopyConstructFrom(other); }

Value &Value::operator=(const Value &other) {
  if (this != &other) {
    Value copy(other);
    DestroyActiveMember();
    MoveConstructFrom(std::move(copy));
    copy.DestroyActiveMember();
  }
  return *this;
}

Value::Value(Value &&other) noexcept : type_(Type::Null) {
  MoveConstructFrom(std::move(other));
  other.DestroyActiveMember();
}

Value &Value::operator=(Value &&other) noexcept {
  if (this != &other) {
    Value moved(std::move(other));
    DestroyActiveMember();
    MoveConstructFrom(std::move(moved));
    moved.DestroyActiveMember();
  }
  return *this;
}

Value::~Value() { DestroyActiveMember(); }

std::ostream &operator<<(std::ostream &os, const Vertex &vertex) {
  os << "(";
  if (vertex.labels.size() > 0) {
    os << ":";
  }
  utils::PrintIterable(os, vertex.labels, ":", [&](auto &stream, auto label) { stream << label; });
  if (vertex.labels.size() > 0 && vertex.properties.size() > 0) {
    os << " ";
  }
  if (vertex.properties.size() > 0) {
    os << "{";
    utils::PrintIterable(os, vertex.properties, ", ", [&](auto &stream, const auto &pair) {
      stream << pair.first << ": " << pair.second;
    });
    os << "}";
  }
  if (!vertex.element_id.empty()) {
    os << " element_id: " << vertex.element_id;
  }
  return os << ")";
}

std::ostream &operator<<(std::ostream &os, const Edge &edge) {
  os << "[" << edge.type;
  if (edge.properties.size() > 0) {
    os << " {";
    utils::PrintIterable(os, edge.properties, ", ", [&](auto &stream, const auto &pair) {
      stream << pair.first << ": " << pair.second;
    });
    os << "}";
  }
  return os << "]";
}

std::ostream &operator<<(std::ostream &os, const UnboundedEdge &edge) {
  os << "[" << edge.type;
  if (edge.properties.size() > 0) {
    os << " {";
    utils::PrintIterable(os, edge.properties, ", ", [&](auto &stream, const auto &pair) {
      stream << pair.first << ": " << pair.second;
    });
    os << "}";
  }
  return os << "]";
}

std::ostream &operator<<(std::ostream &os, const Path &path) {
  if (!path.HasValidStructure()) {
    return os << "<invalid path>";
  }
  os << path.vertices[0];
  for (size_t i = 0; i < path.indices.size(); i += 2) {
    const int64_t edge_ind = path.indices[i];
    const auto vertex_ind = static_cast<size_t>(path.indices[i + 1]);
    const bool arrow_to_right = edge_ind > 0;
    const uint64_t abs_edge =
        arrow_to_right ? static_cast<uint64_t>(edge_ind) : static_cast<uint64_t>(-(edge_ind + 1)) + 1ULL;

    if (!arrow_to_right) os << "<";
    os << "-" << path.edges[static_cast<size_t>(abs_edge - 1ULL)] << "-";
    if (arrow_to_right) os << ">";
    os << path.vertices[vertex_ind];
  }

  return os;
}

std::ostream &operator<<(std::ostream &os, const std::vector<Value> &values) {
  os << "[";
  utils::PrintIterable(os, values);
  return os << "]";
}

std::ostream &operator<<(std::ostream &os, const map_t &values) {
  os << "{";
  utils::PrintIterable(os, values, ", ", [](auto &stream, const auto &pair) {
    stream << pair.first << ": " << pair.second;
  });
  return os << "}";
}

std::ostream &operator<<(std::ostream &os, const Value &value) {
  switch (value.type_) {
    case Value::Type::Null:
      return os << "Null";
    case Value::Type::Bool:
      return os << (value.ValueBool() ? "true" : "false");
    case Value::Type::Int:
      return os << value.ValueInt();
    case Value::Type::Double:
      return os << value.ValueDouble();
    case Value::Type::String:
      return os << utils::Escape(value.ValueString());
    case Value::Type::List:
      return os << value.ValueList();
    case Value::Type::Map:
      return os << value.ValueMap();
    case Value::Type::Vertex:
      return os << value.ValueVertex();
    case Value::Type::Edge:
      return os << value.ValueEdge();
    case Value::Type::UnboundedEdge:
      return os << value.ValueUnboundedEdge();
    case Value::Type::Path:
      return os << value.ValuePath();
    case Value::Type::Date:
      return os << value.ValueDate();
    case Value::Type::LocalTime:
      return os << value.ValueLocalTime();
    case Value::Type::LocalDateTime:
      return os << value.ValueLocalDateTime();
    case Value::Type::Duration:
      return os << value.ValueDuration();
    case Value::Type::ZonedDateTime:
      return os << value.ValueZonedDateTime();
    case Value::Type::Point2d:
      return os << query::CypherConstructionFor(value.ValuePoint2d());
    case Value::Type::Point3d:
      return os << query::CypherConstructionFor(value.ValuePoint3d());
  }
}

std::ostream &operator<<(std::ostream &os, const Value::Type type) {
  switch (type) {
    case Value::Type::Null:
      return os << "null";
    case Value::Type::Bool:
      return os << "bool";
    case Value::Type::Int:
      return os << "int";
    case Value::Type::Double:
      return os << "double";
    case Value::Type::String:
      return os << "string";
    case Value::Type::List:
      return os << "list";
    case Value::Type::Map:
      return os << "map";
    case Value::Type::Vertex:
      return os << "vertex";
    case Value::Type::Edge:
      return os << "edge";
    case Value::Type::UnboundedEdge:
      return os << "unbounded_edge";
    case Value::Type::Path:
      return os << "path";
    case Value::Type::Date:
      return os << "date";
    case Value::Type::LocalTime:
      return os << "local_time";
    case Value::Type::LocalDateTime:
      return os << "local_date_time";
    case Value::Type::Duration:
      return os << "duration";
    case Value::Type::ZonedDateTime:
      return os << "zoned_date_time";
    case Value::Type::Point2d:
      return os << "point_2d";
    case Value::Type::Point3d:
      return os << "point_3d";
  }
}
}  // namespace memgraph::communication::bolt
