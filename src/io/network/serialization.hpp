// Copyright 2022 Memgraph Ltd.
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

#include "io/network/endpoint.hpp"
#include "slk/serialization.hpp"

namespace memgraph::slk {

inline void Save(const io::network::Endpoint &endpoint, Builder *builder) {
  Save(endpoint.GetAddress(), builder);
  Save(endpoint.GetPort(), builder);
}

inline void Load(io::network::Endpoint *endpoint, Reader *reader) {
  std::string address;
  uint16_t port = 0;
  Load(&address, reader);
  Load(&port, reader);
  endpoint->SetAddress(std::move(address));
  endpoint->SetPort(port);
}

}  // namespace memgraph::slk
