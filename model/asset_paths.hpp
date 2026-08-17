// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "model/building.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace imrmf::map_editor {

// Every image the yaml points at, as pointers into the building so callers can
// rewrite in place. Empty references are skipped, a level with no floorplan is
// not broken.
std::vector<std::string *> building_asset_refs(Building &b);
std::vector<const std::string *> building_asset_refs(const Building &b);

// Paths in a building.yaml are relative to the yaml's own directory, so this
// rewrites `stored` to that form where it can. An absolute path outside `base`
// is left alone, breaking it silently would be worse.
std::string relativize_asset_path(const std::string &stored,
                                  const std::filesystem::path &base);

// Whether the path survives moving the map elsewhere. Absolute paths and ones
// escaping via ".." do not.
bool asset_path_is_portable(const std::string &path);

} // namespace imrmf::map_editor
