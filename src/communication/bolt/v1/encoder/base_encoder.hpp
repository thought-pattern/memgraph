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

#include <limits>
#include <string_view>
#include <type_traits>

#include "communication/bolt/v1/codes.hpp"
#include "communication/bolt/v1/value.hpp"
#include "utils/endian.hpp"

static_assert(std::is_same_v<std::uint8_t, char> || std::is_same_v<std::uint8_t, unsigned char>,
              "communication::bolt::Encoder requires uint8_t to be "
              "implemented as char or unsigned char.");

namespace memgraph::communication::bolt {

/**
 * Bolt BaseEncoder. Has public interfaces for writing Bolt encoded data.
 * Supported types are: Null, Bool, Int, Double, String, List, Map, Vertex,
 * Edge, Date, LocalDate, LocalDateTime, Duration, and ZonedDateTime.
 *
 * The purpose of this class is to stream bolt data into the given Buffer.
 *
 * @tparam Buffer the output buffer that should be used
 */
template <typename Buffer>
class BaseEncoder {
 public:
  explicit BaseEncoder(Buffer &buffer) : buffer_(buffer), major_v_(0) {}

  /**
   * Lets the user update the version.
   * This is all single thread for now. TODO: Update if ever multithreaded.
   * @param major_v the major version of the Bolt protocol used.
   */
  void UpdateVersion(int major_v) { major_v_ = major_v; }

  void WriteRAW(const uint8_t *data, uint64_t len) { buffer_.Write(data, len); }

  void WriteRAW(const char *data, uint64_t len) { WriteRAW((const uint8_t *)data, len); }

  void WriteRAW(const uint8_t data) { WriteRAW(&data, 1); }

  void WriteNull() { WriteRAW(std::to_underlying(Marker::Null)); }

  void WriteBool(const bool &value) {
    if (value)
      WriteRAW(std::to_underlying(Marker::True));
    else
      WriteRAW(std::to_underlying(Marker::False));
  }

  void WriteInt(const int64_t &value) {
    if (value >= -16L && value < 128L) {
      WriteRAW(static_cast<uint8_t>(value));
    } else if (value >= -128L && value < -16L) {
      WriteRAW(std::to_underlying(Marker::Int8));
      WriteRAW(static_cast<uint8_t>(value));
    } else if (value >= -32'768L && value < 32'768L) {
      WriteRAW(std::to_underlying(Marker::Int16));
      WritePrimitiveValue(static_cast<int16_t>(value));
    } else if (value >= -2'147'483'648L && value < 2'147'483'648L) {
      WriteRAW(std::to_underlying(Marker::Int32));
      WritePrimitiveValue(static_cast<int32_t>(value));
    } else {
      WriteRAW(std::to_underlying(Marker::Int64));
      WritePrimitiveValue(value);
    }
  }

  void WriteDouble(const double &value) {
    WriteRAW(std::to_underlying(Marker::Float64));
    uint64_t tmp = std::bit_cast<uint64_t>(value);
    WritePrimitiveValue(tmp);
  }

  static constexpr size_t kMaxPackStreamSize = std::numeric_limits<uint32_t>::max();

  [[nodiscard]] static bool CanEncodeSize(size_t size) noexcept { return size <= kMaxPackStreamSize; }

  [[nodiscard]] static bool CanEncodeString(std::string_view value) noexcept { return CanEncodeSize(value.size()); }

  [[nodiscard]] bool CanEncodeList(const std::vector<Value> &value) const noexcept {
    if (!CanEncodeSize(value.size())) return false;
    for (const auto &elem : value) {
      if (!CanEncodeValue(elem)) return false;
    }
    return true;
  }

  [[nodiscard]] bool CanEncodeMap(const map_t &value) const noexcept {
    if (!CanEncodeSize(value.size())) return false;
    for (const auto &[k, v] : value) {
      if (!CanEncodeString(k) || !CanEncodeValue(v)) return false;
    }
    return true;
  }

  [[nodiscard]] bool CanEncodeVertex(const Vertex &vertex) const noexcept {
    if (!CanEncodeSize(vertex.labels.size())) return false;
    for (const auto &label : vertex.labels) {
      if (!CanEncodeString(label)) return false;
    }
    if (!CanEncodeMap(vertex.properties)) return false;
    if (major_v_ > 4 && !CanEncodeString(vertex.element_id)) return false;
    return true;
  }

  [[nodiscard]] bool CanEncodeEdge(const Edge &edge, bool unbound = false) const noexcept {
    if (!CanEncodeString(edge.type) || !CanEncodeMap(edge.properties)) return false;
    if (major_v_ > 4) {
      if (!CanEncodeString(edge.element_id)) return false;
      if (!unbound && (!CanEncodeString(edge.from_element_id) || !CanEncodeString(edge.to_element_id))) return false;
    }
    return true;
  }

  [[nodiscard]] bool CanEncodeEdge(const UnboundedEdge &edge) const noexcept {
    if (!CanEncodeString(edge.type) || !CanEncodeMap(edge.properties)) return false;
    if (major_v_ > 4 && !CanEncodeString(edge.element_id)) return false;
    return true;
  }

  [[nodiscard]] bool CanEncodePath(const Path &path) const noexcept {
    if (!CanEncodeSize(path.vertices.size()) || !CanEncodeSize(path.edges.size()) ||
        !CanEncodeSize(path.indices.size())) {
      return false;
    }
    for (const auto &v : path.vertices) {
      if (!CanEncodeVertex(v)) return false;
    }
    for (const auto &e : path.edges) {
      if (!CanEncodeEdge(e)) return false;
    }
    return true;
  }

  [[nodiscard]] bool CanEncodeValue(const Value &value) const noexcept {
    switch (value.type()) {
      case Value::Type::Null:
      case Value::Type::Bool:
      case Value::Type::Int:
      case Value::Type::Double:
      case Value::Type::Date:
      case Value::Type::LocalTime:
      case Value::Type::LocalDateTime:
      case Value::Type::Duration:
      case Value::Type::Point2d:
      case Value::Type::Point3d:
        return true;
      case Value::Type::String:
        return CanEncodeString(value.ValueString());
      case Value::Type::List:
        return CanEncodeList(value.ValueList());
      case Value::Type::Map:
        return CanEncodeMap(value.ValueMap());
      case Value::Type::Vertex:
        return CanEncodeVertex(value.ValueVertex());
      case Value::Type::Edge:
        return CanEncodeEdge(value.ValueEdge());
      case Value::Type::UnboundedEdge:
        return CanEncodeEdge(value.ValueUnboundedEdge());
      case Value::Type::Path:
        return CanEncodePath(value.ValuePath());
      case Value::Type::ZonedDateTime: {
        const auto &zdt = value.ValueZonedDateTime();
        if (zdt.GetTimezone().InTzDatabase()) {
          return CanEncodeString(zdt.GetTimezone().TimezoneName());
        }
        return true;
      }
    }
    return true;
  }

  void RequireEncodable(bool ok) const {
    if (!ok) {
      throw ValueException("Bolt value exceeds maximum representable 32-bit PackStream length!");
    }
  }

  void WriteTypeSize(const size_t size, const uint8_t typ) {
    RequireEncodable(CanEncodeSize(size));
    if (size <= 15) {
      uint8_t len = size;
      len &= 0x0F;
      WriteRAW(std::to_underlying(MarkerTiny[typ]) + len);
    } else if (size <= 255) {
      uint8_t len = size;
      WriteRAW(std::to_underlying(Marker8[typ]));
      WriteRAW(len);
    } else if (size <= 65'535) {
      uint16_t len = size;
      WriteRAW(std::to_underlying(Marker16[typ]));
      WritePrimitiveValue(len);
    } else {
      auto len = static_cast<uint32_t>(size);
      WriteRAW(std::to_underlying(Marker32[typ]));
      WritePrimitiveValue(len);
    }
  }

  void WriteString(std::string_view value) {
    RequireEncodable(CanEncodeString(value));
    WriteTypeSize(value.size(), MarkerString);
    WriteRAW(value.data(), value.size());
  }

  void WriteList(const std::vector<Value> &value) {
    RequireEncodable(CanEncodeList(value));
    WriteTypeSize(value.size(), MarkerList);
    for (const auto &x : value) WriteValue(x);
  }

  void WriteMap(const map_t &value) {
    RequireEncodable(CanEncodeMap(value));
    WriteTypeSize(value.size(), MarkerMap);
    for (const auto &x : value) {
      WriteString(x.first);
      WriteValue(x.second);
    }
  }

  void WriteVertex(const Vertex &vertex) {
    RequireEncodable(CanEncodeVertex(vertex));
    int struct_n = 3 + 1 * int(major_v_ > 4);  // element_id introduced from v5
    WriteRAW(std::to_underlying(Marker::TinyStruct) + struct_n);
    WriteRAW(std::to_underlying(Signature::Node));
    WriteInt(vertex.id.AsInt());

    // write labels
    const auto &labels = vertex.labels;
    WriteTypeSize(labels.size(), MarkerList);
    for (const auto &label : labels) WriteString(label);

    // write properties
    const auto &props = vertex.properties;
    WriteTypeSize(props.size(), MarkerMap);
    for (const auto &prop : props) {
      WriteString(prop.first);
      WriteValue(prop.second);
    }

    if (major_v_ > 4) {
      // element_id introduced in v5.0
      WriteString(vertex.element_id);
    }
  }

  void WriteEdge(const Edge &edge, bool unbound = false) {
    RequireEncodable(CanEncodeEdge(edge, unbound));
    int struct_n = (unbound ? 3 + 1 * int(major_v_ > 4) : 5 + 3 * int(major_v_ > 4));  // element_id introduced from v5
    WriteRAW(std::to_underlying(Marker::TinyStruct) + struct_n);
    WriteRAW(std::to_underlying(unbound ? Signature::UnboundRelationship : Signature::Relationship));

    WriteInt(edge.id.AsInt());
    if (!unbound) {
      WriteInt(edge.from.AsInt());
      WriteInt(edge.to.AsInt());
    }

    WriteString(edge.type);

    const auto &props = edge.properties;
    WriteTypeSize(props.size(), MarkerMap);
    for (const auto &prop : props) {
      WriteString(prop.first);
      WriteValue(prop.second);
    }

    if (major_v_ > 4) {
      // element_id introduced in v5.0
      WriteString(edge.element_id);
      if (!unbound) {
        // from_element_id introduced in v5.0
        WriteString(edge.from_element_id);
        // to_element_id introduced in v5.0
        WriteString(edge.to_element_id);
      }
    }
  }

  void WriteEdge(const UnboundedEdge &edge) {
    RequireEncodable(CanEncodeEdge(edge));
    const int struct_n = 3 + 1 * int(major_v_ > 4);  // element_id introduced from v5
    WriteRAW(std::to_underlying(Marker::TinyStruct) + struct_n);
    WriteRAW(std::to_underlying(Signature::UnboundRelationship));

    WriteInt(edge.id.AsInt());

    WriteString(edge.type);

    const auto &props = edge.properties;
    WriteTypeSize(props.size(), MarkerMap);
    for (const auto &prop : props) {
      WriteString(prop.first);
      WriteValue(prop.second);
    }

    if (major_v_ > 4) {
      // element_id introduced in v5.0
      WriteString(edge.element_id);
    }
  }

  void WritePath(const Path &path) {
    RequireEncodable(CanEncodePath(path));
    WriteRAW(std::to_underlying(Marker::TinyStruct) + 3);
    WriteRAW(std::to_underlying(Signature::Path));
    WriteTypeSize(path.vertices.size(), MarkerList);
    for (const auto &v : path.vertices) WriteVertex(v);
    WriteTypeSize(path.edges.size(), MarkerList);
    for (const auto &e : path.edges) WriteEdge(e);
    WriteTypeSize(path.indices.size(), MarkerList);
    for (const auto &i : path.indices) WriteInt(i);
  }

  void WriteDate(const utils::Date &date) {
    WriteRAW(std::to_underlying(Marker::TinyStruct1));
    WriteRAW(std::to_underlying(Signature::Date));
    WriteInt(date.DaysSinceEpoch());
  }

  void WriteLocalTime(const utils::LocalTime &local_time) {
    WriteRAW(std::to_underlying(Marker::TinyStruct1));
    WriteRAW(std::to_underlying(Signature::LocalTime));
    WriteInt(local_time.NanosecondsSinceEpoch());
  }

  void WriteLocalDateTime(const utils::LocalDateTime &local_date_time) {
    // Bolt defines local date time without timezone (as if UTC)
    WriteRAW(std::to_underlying(Marker::TinyStruct2));
    WriteRAW(std::to_underlying(Signature::LocalDateTime));
    WriteInt(local_date_time.SecondsSinceEpoch());
    WriteInt(local_date_time.SubSecondsAsNanoseconds());
  }

  void WriteDuration(const utils::Duration &duration) {
    WriteRAW(std::to_underlying(Marker::TinyStruct4));
    WriteRAW(std::to_underlying(Signature::Duration));
    // This shall always be zero because internally we store microseconds
    // and converting months to microseconds is an approximation. However,
    // for the encoder, we implement ReadInt() to support the neo4j driver.
    WriteInt(0);
    WriteInt(duration.Days());
    WriteInt(duration.SubDaysAsSeconds());
    WriteInt(duration.SubSecondsAsNanoseconds());
  }

  void WritePoint2d(const Point2d &point_2d) {
    WriteRAW(std::to_underlying(Marker::TinyStruct3));
    WriteRAW(std::to_underlying(Signature::Point2d));
    WriteInt(storage::CrsToSrid(point_2d.crs()).value_of());
    WriteDouble(point_2d.x());
    WriteDouble(point_2d.y());
  }

  void WritePoint3d(const Point3d &point_3d) {
    WriteRAW(std::to_underlying(Marker::TinyStruct4));
    WriteRAW(std::to_underlying(Signature::Point3d));
    WriteInt(storage::CrsToSrid(point_3d.crs()).value_of());
    WriteDouble(point_3d.x());
    WriteDouble(point_3d.y());
    WriteDouble(point_3d.z());
  }

  void WriteLegacyZonedDateTime(const utils::ZonedDateTime &zoned_date_time) {
    WriteRAW(std::to_underlying(Marker::TinyStruct3));
    if (zoned_date_time.GetTimezone().InTzDatabase()) {
      WriteRAW(std::to_underlying(Signature::LegacyDateTimeZoneId));
      WriteInt((zoned_date_time.SysSecondsSinceEpoch() + zoned_date_time.OffsetDuration()).count());
      WriteInt(zoned_date_time.SysSubSecondsAsNanoseconds().count());
      WriteString(std::string{zoned_date_time.GetTimezone().TimezoneName()});
    } else {
      WriteRAW(std::to_underlying(Signature::LegacyDateTime));
      WriteInt((zoned_date_time.SysSecondsSinceEpoch() + zoned_date_time.OffsetDuration()).count());
      WriteInt(zoned_date_time.SysSubSecondsAsNanoseconds().count());
      WriteInt(zoned_date_time.GetTimezone().DefiningOffsetSeconds());
    }
  }

  void WriteV5ZonedDateTime(const utils::ZonedDateTime &zoned_date_time) {
    WriteRAW(std::to_underlying(Marker::TinyStruct3));
    if (zoned_date_time.GetTimezone().InTzDatabase()) {
      WriteRAW(std::to_underlying(Signature::DateTimeZoneId));
      WriteInt(zoned_date_time.SysSecondsSinceEpoch().count());
      WriteInt(zoned_date_time.SysSubSecondsAsNanoseconds().count());
      WriteString(std::string{zoned_date_time.GetTimezone().TimezoneName()});
    } else {
      WriteRAW(std::to_underlying(Signature::DateTime));
      WriteInt(zoned_date_time.SysSecondsSinceEpoch().count());
      WriteInt(zoned_date_time.SysSubSecondsAsNanoseconds().count());
      WriteInt(zoned_date_time.GetTimezone().DefiningOffsetSeconds());
    }
  }

  void WriteZonedDateTime(const utils::ZonedDateTime &zoned_date_time) {
    if (major_v_ > 4) {
      WriteV5ZonedDateTime(zoned_date_time);
    } else {
      WriteLegacyZonedDateTime(zoned_date_time);
    }
  }

  void WriteValue(const Value &value) {
    switch (value.type()) {
      case Value::Type::Null:
        WriteNull();
        break;
      case Value::Type::Bool:
        WriteBool(value.ValueBool());
        break;
      case Value::Type::Int:
        WriteInt(value.ValueInt());
        break;
      case Value::Type::Double:
        WriteDouble(value.ValueDouble());
        break;
      case Value::Type::String:
        WriteString(value.ValueString());
        break;
      case Value::Type::List:
        WriteList(value.ValueList());
        break;
      case Value::Type::Map:
        WriteMap(value.ValueMap());
        break;
      case Value::Type::Vertex:
        WriteVertex(value.ValueVertex());
        break;
      case Value::Type::Edge:
        WriteEdge(value.ValueEdge());
        break;
      case Value::Type::UnboundedEdge:
        WriteEdge(value.ValueUnboundedEdge());
        break;
      case Value::Type::Path:
        WritePath(value.ValuePath());
        break;
      case Value::Type::Date:
        WriteDate(value.ValueDate());
        break;
      case Value::Type::LocalTime:
        WriteLocalTime(value.ValueLocalTime());
        break;
      case Value::Type::LocalDateTime:
        WriteLocalDateTime(value.ValueLocalDateTime());
        break;
      case Value::Type::ZonedDateTime:
        WriteZonedDateTime(value.ValueZonedDateTime());
        break;
      case Value::Type::Duration:
        WriteDuration(value.ValueDuration());
        break;
      case Value::Type::Point2d:
        WritePoint2d(value.ValuePoint2d());
        break;
      case Value::Type::Point3d:
        WritePoint3d(value.ValuePoint3d());
        break;
    }
  }

 protected:
  Buffer &buffer_;
  int major_v_;  //!< Major version of the underlying Bolt protocol (TODO: Think about reimplementing the versioning)

 private:
  template <class T>
  void WritePrimitiveValue(T value) {
    value = utils::HostToBigEndian(value);
    WriteRAW(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
  }
};

}  // namespace memgraph::communication::bolt
