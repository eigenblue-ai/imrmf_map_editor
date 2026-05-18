// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "model/yaml_io.hpp"

#include "yaml-cpp/yaml.h"
#include <sstream>

namespace imrmf::map_editor {

namespace {

// rmf_traffic_editor encodes param values as a 2-element sequence:
//   [type_code, value]   where 1=string, 2=int, 3=double, 4=bool
ParamValue param_from_yaml(const YAML::Node &node) {
  ParamValue p;
  if (node.IsSequence() && node.size() >= 2) {
    int code = node[0].as<int>();
    const YAML::Node &v = node[1];
    switch (code) {
    case 1:
      p.type = ParamType::STRING;
      p.s = v.as<std::string>("");
      break;
    case 2:
      p.type = ParamType::INT;
      p.i = v.as<int>(0);
      break;
    case 3:
      p.type = ParamType::DOUBLE;
      p.d = v.as<double>(0.0);
      break;
    case 4:
      p.type = ParamType::BOOL;
      p.b = v.as<bool>(false);
      break;
    default:
      p.type = ParamType::STRING;
      p.s = v.as<std::string>("");
      break;
    }
    return p;
  }
  // Older form: plain scalar — infer type.
  if (node.IsScalar()) {
    const std::string raw = node.Scalar();
    try {
      p.type = ParamType::BOOL;
      p.b = node.as<bool>();
      return p;
    } catch (...) {
    }
    try {
      p.type = ParamType::INT;
      p.i = node.as<int>();
      return p;
    } catch (...) {
    }
    try {
      p.type = ParamType::DOUBLE;
      p.d = node.as<double>();
      return p;
    } catch (...) {
    }
    p.type = ParamType::STRING;
    p.s = raw;
  }
  return p;
}

YAML::Node param_to_yaml(const ParamValue &p) {
  YAML::Node n(YAML::NodeType::Sequence);
  n.SetStyle(YAML::EmitterStyle::Flow);
  n.push_back(static_cast<int>(p.type));
  switch (p.type) {
  case ParamType::STRING:
    n.push_back(p.s);
    break;
  case ParamType::INT:
    n.push_back(p.i);
    break;
  case ParamType::DOUBLE:
    n.push_back(p.d);
    break;
  case ParamType::BOOL:
    n.push_back(p.b);
    break;
  }
  return n;
}

Vertex vertex_from_yaml(const YAML::Node &node) {
  if (!node.IsSequence() || node.size() < 2) {
    throw YamlParseError(
        "vertex node must be a sequence with at least 2 entries");
  }
  Vertex v;
  v.x = node[0].as<double>();
  v.y = node[1].as<double>();
  if (node.size() >= 3)
    v.z = node[2].as<double>(0.0);
  if (node.size() >= 4)
    v.name = node[3].as<std::string>("");
  if (node.size() >= 5 && node[4].IsMap()) {
    for (auto it = node[4].begin(); it != node[4].end(); ++it) {
      v.params[it->first.as<std::string>()] = param_from_yaml(it->second);
    }
  }
  return v;
}

YAML::Node vertex_to_yaml(const Vertex &v) {
  YAML::Node n(YAML::NodeType::Sequence);
  n.SetStyle(YAML::EmitterStyle::Flow);
  n.push_back(v.x);
  n.push_back(v.y);
  n.push_back(v.z);
  n.push_back(v.name);
  YAML::Node params(YAML::NodeType::Map);
  for (const auto &[k, val] : v.params) {
    params[k] = param_to_yaml(val);
  }
  n.push_back(params);
  return n;
}

Lane lane_from_yaml(const YAML::Node &node) {
  if (!node.IsSequence() || node.size() < 2) {
    throw YamlParseError(
        "lane node must be a sequence with at least 2 entries");
  }
  Lane l;
  l.start_idx = node[0].as<int>();
  l.end_idx = node[1].as<int>();
  if (node.size() >= 3 && node[2].IsMap()) {
    for (auto it = node[2].begin(); it != node[2].end(); ++it) {
      l.params[it->first.as<std::string>()] = param_from_yaml(it->second);
    }
  }
  return l;
}

YAML::Node lane_to_yaml(const Lane &l) {
  YAML::Node n(YAML::NodeType::Sequence);
  n.SetStyle(YAML::EmitterStyle::Flow);
  n.push_back(l.start_idx);
  n.push_back(l.end_idx);
  YAML::Node params(YAML::NodeType::Map);
  for (const auto &[k, val] : l.params) {
    params[k] = param_to_yaml(val);
  }
  n.push_back(params);
  return n;
}

Layer layer_from_yaml(const std::string &name, const YAML::Node &node) {
  Layer l;
  l.name = name;
  if (!node.IsMap())
    return l;
  if (node["filename"])
    l.filename = node["filename"].as<std::string>("");
  if (node["visible"])
    l.visible = node["visible"].as<bool>(true);
  if (node["color"] && node["color"].IsSequence() &&
      node["color"].size() >= 4) {
    l.color_r = node["color"][0].as<double>(1.0);
    l.color_g = node["color"][1].as<double>(1.0);
    l.color_b = node["color"][2].as<double>(1.0);
    l.color_a = node["color"][3].as<double>(0.5);
  }
  if (node["transform"] && node["transform"].IsMap()) {
    const YAML::Node &t = node["transform"];
    if (t["scale"])
      l.scale = t["scale"].as<double>(1.0);
    if (t["yaw"])
      l.yaw = t["yaw"].as<double>(0.0);
    if (t["translation_x"])
      l.translation_x = t["translation_x"].as<double>(0.0);
    if (t["translation_y"])
      l.translation_y = t["translation_y"].as<double>(0.0);
  }
  // Preserve features and any other unknown keys.
  l.passthrough = YAML::Node(YAML::NodeType::Map);
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    if (key == "filename" || key == "visible" || key == "color" ||
        key == "transform")
      continue;
    l.passthrough[key] = it->second;
  }
  return l;
}

YAML::Node layer_to_yaml(const Layer &l) {
  YAML::Node n(YAML::NodeType::Map);
  // Splice unknowns first so known keys take precedence on overlap.
  if (l.passthrough && l.passthrough.IsMap()) {
    for (auto it = l.passthrough.begin(); it != l.passthrough.end(); ++it) {
      n[it->first.as<std::string>()] = it->second;
    }
  }
  n["filename"] = l.filename;
  n["visible"] = l.visible;
  YAML::Node color(YAML::NodeType::Sequence);
  color.SetStyle(YAML::EmitterStyle::Flow);
  color.push_back(l.color_r);
  color.push_back(l.color_g);
  color.push_back(l.color_b);
  color.push_back(l.color_a);
  n["color"] = color;
  YAML::Node t(YAML::NodeType::Map);
  t["scale"] = l.scale;
  t["yaw"] = l.yaw;
  t["translation_x"] = l.translation_x;
  t["translation_y"] = l.translation_y;
  n["transform"] = t;
  return n;
}

// Known top-level level keys we model directly. Everything else is passthrough.
const char *kKnownLevelKeys[] = {"vertices", "lanes", "elevation", "drawing",
                                 "layers"};

bool is_known_level_key(const std::string &k) {
  for (const char *known : kKnownLevelKeys)
    if (k == known)
      return true;
  return false;
}

Level level_from_yaml(const std::string &name, const YAML::Node &node) {
  Level lvl;
  lvl.name = name;
  if (node["elevation"])
    lvl.elevation = node["elevation"].as<double>(0.0);
  if (node["drawing"] && node["drawing"]["filename"]) {
    lvl.drawing_filename = node["drawing"]["filename"].as<std::string>("");
  }
  if (node["vertices"]) {
    for (const auto &v : node["vertices"])
      lvl.vertices.push_back(vertex_from_yaml(v));
  }
  if (node["lanes"]) {
    for (const auto &l : node["lanes"])
      lvl.lanes.push_back(lane_from_yaml(l));
  }
  if (node["layers"] && node["layers"].IsMap()) {
    for (auto it = node["layers"].begin(); it != node["layers"].end(); ++it) {
      lvl.layers.push_back(
          layer_from_yaml(it->first.as<std::string>(), it->second));
    }
  }
  // Stash everything else verbatim so it round-trips.
  lvl.passthrough = YAML::Node(YAML::NodeType::Map);
  for (auto it = node.begin(); it != node.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    if (!is_known_level_key(key))
      lvl.passthrough[key] = it->second;
  }
  return lvl;
}

YAML::Node level_to_yaml(const Level &lvl) {
  YAML::Node n(YAML::NodeType::Map);
  // Splice passthrough first so known keys take precedence on overlap.
  if (lvl.passthrough && lvl.passthrough.IsMap()) {
    for (auto it = lvl.passthrough.begin(); it != lvl.passthrough.end(); ++it) {
      n[it->first.as<std::string>()] = it->second;
    }
  }
  if (!lvl.drawing_filename.empty()) {
    YAML::Node drawing(YAML::NodeType::Map);
    drawing["filename"] = lvl.drawing_filename;
    n["drawing"] = drawing;
  }
  n["elevation"] = lvl.elevation;
  YAML::Node verts(YAML::NodeType::Sequence);
  for (const auto &v : lvl.vertices)
    verts.push_back(vertex_to_yaml(v));
  n["vertices"] = verts;
  YAML::Node lanes(YAML::NodeType::Sequence);
  for (const auto &l : lvl.lanes)
    lanes.push_back(lane_to_yaml(l));
  n["lanes"] = lanes;
  if (!lvl.layers.empty()) {
    YAML::Node layers(YAML::NodeType::Map);
    for (const auto &L : lvl.layers)
      layers[L.name] = layer_to_yaml(L);
    n["layers"] = layers;
  }
  return n;
}

const char *kKnownBuildingKeys[] = {"name", "coordinate_system", "levels"};

bool is_known_building_key(const std::string &k) {
  for (const char *known : kKnownBuildingKeys)
    if (k == known)
      return true;
  return false;
}

} // namespace

Building parse_building(const std::string &yaml_text) {
  YAML::Node root;
  try {
    root = YAML::Load(yaml_text);
  } catch (const YAML::Exception &e) {
    throw YamlParseError(std::string("yaml load failed: ") + e.what());
  }
  if (!root.IsMap())
    throw YamlParseError("building yaml root must be a map");

  Building b;
  if (root["name"])
    b.name = root["name"].as<std::string>("");
  if (root["coordinate_system"])
    b.coordinate_system = root["coordinate_system"].as<std::string>("");

  if (root["levels"] && root["levels"].IsMap()) {
    for (auto it = root["levels"].begin(); it != root["levels"].end(); ++it) {
      b.levels.push_back(
          level_from_yaml(it->first.as<std::string>(), it->second));
    }
  }

  b.passthrough = YAML::Node(YAML::NodeType::Map);
  for (auto it = root.begin(); it != root.end(); ++it) {
    const std::string key = it->first.as<std::string>();
    if (!is_known_building_key(key))
      b.passthrough[key] = it->second;
  }
  return b;
}

std::string serialize_vertex(const Vertex &v) {
  YAML::Emitter emitter;
  emitter << vertex_to_yaml(v);
  return std::string(emitter.c_str() ? emitter.c_str() : "");
}

std::string serialize_lane(const Lane &l) {
  YAML::Emitter emitter;
  emitter << lane_to_yaml(l);
  return std::string(emitter.c_str() ? emitter.c_str() : "");
}

std::string serialize_layer(const Layer &l) {
  YAML::Emitter emitter;
  emitter << layer_to_yaml(l);
  return std::string(emitter.c_str() ? emitter.c_str() : "");
}

std::string serialize_building(const Building &b) {
  YAML::Node root(YAML::NodeType::Map);
  if (b.passthrough && b.passthrough.IsMap()) {
    for (auto it = b.passthrough.begin(); it != b.passthrough.end(); ++it) {
      root[it->first.as<std::string>()] = it->second;
    }
  }
  if (!b.coordinate_system.empty())
    root["coordinate_system"] = b.coordinate_system;
  root["name"] = b.name;
  YAML::Node levels(YAML::NodeType::Map);
  for (const Level &lvl : b.levels)
    levels[lvl.name] = level_to_yaml(lvl);
  root["levels"] = levels;

  YAML::Emitter emitter;
  emitter << root;
  std::ostringstream oss;
  oss << emitter.c_str() << "\n";
  return oss.str();
}

} // namespace imrmf::map_editor
