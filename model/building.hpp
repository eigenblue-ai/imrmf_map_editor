// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "yaml-cpp/yaml.h"
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace imrmf::map_editor {

// rmf param type codes from building.yaml.
enum class ParamType : int {
  STRING = 1,
  INT = 2,
  DOUBLE = 3,
  BOOL = 4,
};

struct ParamValue {
  ParamType type = ParamType::STRING;
  std::string s;
  int i = 0;
  double d = 0.0;
  bool b = false;

  static ParamValue make_string(std::string v) {
    ParamValue p;
    p.type = ParamType::STRING;
    p.s = std::move(v);
    return p;
  }
  static ParamValue make_int(int v) {
    ParamValue p;
    p.type = ParamType::INT;
    p.i = v;
    return p;
  }
  static ParamValue make_double(double v) {
    ParamValue p;
    p.type = ParamType::DOUBLE;
    p.d = v;
    return p;
  }
  static ParamValue make_bool(bool v) {
    ParamValue p;
    p.type = ParamType::BOOL;
    p.b = v;
    return p;
  }
};

struct Vertex {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  std::string name;
  std::map<std::string, ParamValue> params;
};

struct Lane {
  int start_idx = 0;
  int end_idx = 0;
  std::map<std::string, ParamValue> params;
};

// rmf_traffic_editor layer. Transform fields are meters / radians.
struct Layer {
  std::string name;
  std::string filename;
  bool visible = true;
  double color_r = 1.0, color_g = 1.0, color_b = 1.0, color_a = 0.5;
  double scale = 1.0;         // meters per image-pixel
  double yaw = 0.0;           // radians
  double translation_x = 0.0; // meters
  double translation_y = 0.0; // meters
  YAML::Node passthrough;     // features and anything else
};

struct Level {
  std::string name;
  double elevation = 0.0;
  std::string drawing_filename;
  std::vector<Vertex> vertices;
  std::vector<Lane> lanes;
  std::vector<Layer> layers;
  YAML::Node passthrough; // walls, fiducials, measurements, doors, ...
};

struct Building {
  std::string name;
  std::string coordinate_system;
  std::vector<Level> levels;
  YAML::Node passthrough; // graphs, lifts, crowd_sim, ...
};

// Meters / pixel
double compute_level_mpp(const Building &building, int level_idx);

std::vector<int> lanes_referencing_vertex(const Level &level, int vertex_idx);

void delete_vertex(Level &level, int vertex_idx);

void delete_lane(Level &level, int lane_idx);

} // namespace imrmf::map_editor
