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

#include "communication/bolt/client.hpp"

#include <array>

#include "communication/bolt/v1/codes.hpp"
#include "communication/bolt/v1/mg_types.hpp"
#include "communication/bolt/v1/value.hpp"
#include "utils/logging.hpp"

#include "communication/bolt/v1/fmt.hpp"
#include "io/network/fmt.hpp"

namespace {
constexpr auto kBoltV43Version = std::array<uint8_t, 4>{0x00, 0x00, 0x03, 0x04};
constexpr auto kEmptyBoltVersion = std::array<uint8_t, 4>{0x00, 0x00, 0x00, 0x00};
}  // namespace

namespace memgraph::communication::bolt {

Client::Client(communication::ClientContext &context) : client_{&context} {}

void Client::Connect(const io::network::Endpoint &endpoint, const std::string &username, const std::string &password,
                     const std::string &client_name) {
  encoder_buffer_.Clear();
  if (!client_.Connect(endpoint)) {
    throw ClientFatalException("Couldn't connect to {}!", endpoint.SocketAddress());
  }

  if (!client_.Write(kPreamble, true)) {
    spdlog::error("Couldn't send preamble!");
    Close();
    throw ServerCommunicationException();
  }

  if (!client_.Write(kBoltV43Version, true)) {
    spdlog::error("Couldn't send protocol version!");
    Close();
    throw ServerCommunicationException();
  }

  for (int i = 0; i < 3; ++i) {
    if (!client_.Write(kEmptyBoltVersion, i != 2)) {
      spdlog::error("Couldn't send protocol version!");
      Close();
      throw ServerCommunicationException();
    }
  }

  if (!client_.Read(kBoltV43Version.size())) {
    spdlog::error("Couldn't get negotiated protocol version!");
    Close();
    throw ServerCommunicationException();
  }

  if (memcmp(kBoltV43Version.data(), client_.GetData(), kBoltV43Version.size()) != 0) {
    spdlog::error("Server negotiated unsupported protocol version!");
    Close();
    throw ClientFatalException("The server negotiated an usupported protocol version!");
  }
  client_.ShiftData(kBoltV43Version.size());
  decoder_.UpdateVersion(4);
  encoder_.UpdateVersion(4);

  if (!encoder_.MessageInit({{"user_agent", client_name},
                             {"scheme", "basic"},
                             {"principal", username},
                             {"credentials", password},
                             {"routing", {}}})) {
    spdlog::error("Couldn't send init message!");
    Close();
    throw ServerCommunicationException();
  }

  Signature signature{};
  Value metadata;
  if (!ReadMessage(signature, metadata)) {
    spdlog::error("Couldn't read init message response!");
    Close();
    throw ServerCommunicationException();
  }
  if (signature != Signature::Success) {
    spdlog::error("Handshake failed!");
    Close();
    throw ClientFatalException("Handshake with the server failed!");
  }

  spdlog::debug("Metadata of init message response: {}", metadata);
}

QueryData Client::Execute(std::string_view query, const map_t &parameters, const map_t &extra) {
  if (!client_.IsConnected()) {
    throw ClientFatalException("You must first connect to the server before using the client!");
  }

  if (spdlog::should_log(spdlog::level::debug)) {
    const auto [redacted_query, redacted_params] = RedactBoltQueryAndParams(query, parameters);
    spdlog::debug("Sending run message with statement: '{}'; parameters: {}", redacted_query, redacted_params);
  }

  // It is super critical from performance point of view to send the pull message right after the run message. Otherwise
  // the performance will degrade multiple magnitudes.
  if (!encoder_.MessageRun(query, parameters, extra) || !encoder_.MessagePull({{"n", Value(-1)}})) {
    spdlog::error("Couldn't send run/pull request!");
    Close();
    throw ServerCommunicationException();
  }

  spdlog::debug("Reading run message response");
  Signature signature{};
  Value fields;
  if (!ReadMessage(signature, fields)) {
    Close();
    throw ServerCommunicationException();
  }
  if (fields.type() != Value::Type::Map) {
    Close();
    throw ServerMalformedDataException();
  }

  if (signature == Signature::Failure) {
    HandleFailure<ClientQueryException>(fields.ValueMap());
  }
  if (signature != Signature::Success) {
    Close();
    throw ServerMalformedDataException();
  }

  spdlog::debug("Reading pull_all message response");
  Marker marker{};
  Value metadata;
  std::vector<std::vector<Value>> records;
  while (true) {
    if (!GetMessage()) {
      Close();
      throw ServerCommunicationException();
    }
    if (!decoder_.ReadMessageHeader(&signature, &marker)) {
      Close();
      throw ServerCommunicationException();
    }
    if (signature == Signature::Record) {
      Value record;
      if (!decoder_.ReadValue(&record, Value::Type::List)) {
        Close();
        throw ServerCommunicationException();
      }
      records.emplace_back(std::move(record.ValueList()));
    } else if (signature == Signature::Success) {
      if (!decoder_.ReadValue(&metadata)) {
        Close();
        throw ServerCommunicationException();
      }
      break;
    } else if (signature == Signature::Failure) {
      Value data;
      if (!decoder_.ReadValue(&data, Value::Type::Map)) {
        Close();
        throw ServerCommunicationException();
      }
      HandleFailure<ClientQueryException>(data.ValueMap());
    } else {
      Close();
      throw ServerMalformedDataException();
    }
  }

  if (metadata.type() != Value::Type::Map) {
    Close();
    throw ServerMalformedDataException();
  }

  auto &header = fields.ValueMap();

  QueryData ret{{}, std::move(records), std::move(metadata.ValueMap())};

  auto fields_it = header.find("fields");
  if (fields_it == header.end() || fields_it->second.type() != Value::Type::List) {
    Close();
    throw ServerMalformedDataException();
  }
  auto &field_vector = fields_it->second.ValueList();

  for (auto &field_item : field_vector) {
    if (field_item.type() != Value::Type::String) {
      Close();
      throw ServerMalformedDataException();
    }
    ret.fields.emplace_back(std::move(field_item.ValueString()));
  }

  auto qid_it = header.find("qid");
  if (qid_it != header.end()) {
    ret.metadata["qid"] = qid_it->second;
  }

  return ret;
}

void Client::Reset() {
  if (!client_.IsConnected()) {
    throw ClientFatalException("You must first connect to the server before using the client!");
  }

  spdlog::debug("Sending reset message");

  if (!encoder_.MessageReset()) {
    spdlog::error("Couldn't send reset message!");
    Close();
    throw ServerCommunicationException();
  }

  Signature signature{};
  Value fields;
  // In Execute the pull message is sent right after the run message without reading the answer for the run message.
  // That means some of the messages sent might get ignored.
  while (true) {
    if (!ReadMessage(signature, fields)) {
      Close();
      throw ServerCommunicationException();
    }
    if (signature == Signature::Success) {
      break;
    }
    if (signature != Signature::Ignored) {
      Close();
      throw ServerMalformedDataException();
    }
  }
}

std::optional<map_t> Client::Route(const map_t &routing, const std::vector<Value> &bookmarks,
                                   const std::optional<std::string> &db) {
  if (!client_.IsConnected()) {
    throw ClientFatalException("You must first connect to the server before using the client!");
  }

  if (spdlog::should_log(spdlog::level::debug)) {
    const auto redacted_routing = RedactBoltValue(Value{routing});
    spdlog::debug("Sending route message with routing: {}; bookmarks: {}; db: {}",
                  redacted_routing.ValueMap(),
                  bookmarks,
                  db.has_value() ? Value(*db) : Value());
  }

  if (!encoder_.MessageRoute(routing, bookmarks, db)) {
    spdlog::error("Couldn't send route message!");
    Close();
    throw ServerCommunicationException();
  }

  spdlog::debug("Reading route message response");
  Signature signature{};
  Value fields;
  if (!ReadMessage(signature, fields) || fields.type() != Value::Type::Map) {
    Close();
    throw ServerCommunicationException();
  }
  if (signature == Signature::Ignored) {
    return std::nullopt;
  }
  if (signature == Signature::Failure) {
    HandleFailure(fields.ValueMap());
  }
  if (signature != Signature::Success) {
    Close();
    throw ServerMalformedDataException{};
  }
  return fields.ValueMap();
}

void Client::Close() {
  encoder_buffer_.Clear();
  client_.ClearData();
  client_.Close();
}

bool Client::GetMessage() {
  client_.ClearData();
  while (true) {
    if (!client_.Read(kChunkHeaderSize)) return false;

    size_t chunk_size = client_.GetData()[0];
    chunk_size <<= 8U;
    chunk_size += client_.GetData()[1];
    if (chunk_size == 0) return true;

    if (!client_.Read(chunk_size)) return false;
    if (decoder_buffer_.GetChunk() != ChunkState::Whole) return false;
    client_.ClearData();
  }
  return true;
}

bool Client::ReadMessage(Signature &signature, Value &ret) {
  Marker marker{};
  if (!GetMessage()) return false;
  if (!decoder_.ReadMessageHeader(&signature, &marker)) return false;
  return ReadMessageData(marker, ret);
}

bool Client::ReadMessageData(Marker marker, Value &ret) {
  if (marker == Marker::TinyStruct) {
    ret = Value();
    return true;
  }
  if (marker == Marker::TinyStruct1) {
    return decoder_.ReadValue(&ret);
  }
  return false;
}

}  // namespace memgraph::communication::bolt
