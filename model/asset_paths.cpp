// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "model/asset_paths.hpp"

namespace imrmf::map_editor {

namespace fs = std::filesystem;

namespace {

// Same walk for both const and non-const buildings.
template <typename BuildingT, typename StringT>
std::vector<StringT *> collect_refs(BuildingT &b) {
  std::vector<StringT *> out;
  for (auto &level : b.levels) {
    if (!level.drawing_filename.empty())
      out.push_back(&level.drawing_filename);
    for (auto &layer : level.layers) {
      if (!layer.filename.empty())
        out.push_back(&layer.filename);
    }
  }
  return out;
}

std::string to_generic(const fs::path &p) { return p.generic_string(); }

} // namespace

std::vector<std::string *> building_asset_refs(Building &b) {
  return collect_refs<Building, std::string>(b);
}

std::vector<const std::string *> building_asset_refs(const Building &b) {
  return collect_refs<const Building, const std::string>(b);
}

std::string relativize_asset_path(const std::string &stored,
                                  const fs::path &base) {
  if (stored.empty())
    return stored;

  fs::path p = fs::path(stored).lexically_normal();
  if (p.is_relative()) {
    std::string s = to_generic(p);
    // lexically_normal leaves a trailing "." on paths like "foo/".
    if (s == ".")
      return stored;
    return s;
  }

  if (base.empty())
    return to_generic(p);

  fs::path rel = p.lexically_relative(base.lexically_normal());
  if (rel.empty())
    return to_generic(p);
  std::string s = to_generic(rel);
  // Escaping the base is not an improvement over the absolute path.
  if (!asset_path_is_portable(s))
    return to_generic(p);
  return s;
}

bool asset_path_is_portable(const std::string &path) {
  if (path.empty())
    return true;
  fs::path p(path);
  if (p.is_absolute())
    return false;
  int depth = 0;
  for (const auto &part : p) {
    const std::string s = part.string();
    if (s == "..") {
      if (--depth < 0)
        return false;
    } else if (s != "." && !s.empty()) {
      ++depth;
    }
  }
  return true;
}

} // namespace imrmf::map_editor
