// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "model/building.hpp"
#include <stdexcept>
#include <string>

namespace imrmf::map_editor {

class YamlParseError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

Building parse_building(const std::string &yaml_text);
std::string serialize_building(const Building &b);

// Single-element emitters for fine-grained CRDT ops.
std::string serialize_vertex(const Vertex &v);
std::string serialize_lane(const Lane &l);
std::string serialize_layer(const Layer &l);

} // namespace imrmf::map_editor
