// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "model/building.hpp"

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace imrmf::map_editor {

// A whole map in one plain zip, extension .rmfmap:
//
//   manifest.json          format / version / yaml name / path -> entry table
//   <id>.building.yaml     byte-identical to serialize_building()
//   assets/...             one entry per referenced image
//
// The yaml carries no bundle-only keys, so an unzipped bundle is an ordinary
// map directory.

inline constexpr const char *kBundleExtension = ".rmfmap";
inline constexpr const char *kBundleFormat = "rmfmap";
inline constexpr int kBundleVersion = 1;

class BundleError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct BundleAsset {
  std::string path;  // as referenced from the yaml
  std::string entry; // where the bytes live in the archive
  std::vector<unsigned char> bytes;
};

struct MapBundle {
  std::string yaml_name;
  Building building;
  std::vector<BundleAsset> assets;
  std::vector<std::string> missing; // referenced, but no bytes to be had
};

// Fetches the bytes behind one yaml-relative asset path. False lands the path
// in MapBundle::missing rather than failing the pack.
using AssetReader =
    std::function<bool(const std::string &path, std::vector<unsigned char> *)>;

// Pulls every asset reference through `read` and assigns archive entry names.
// Does not touch the filesystem itself.
MapBundle collect_bundle(const Building &building, std::string yaml_name,
                         const AssetReader &read);

std::vector<unsigned char> write_bundle(const MapBundle &bundle);

// Throws BundleError on anything malformed: not a zip, no manifest, wrong
// format or version, an entry escaping the archive root, or oversized input.
MapBundle read_bundle(const unsigned char *data, std::size_t len);

const std::vector<unsigned char> *find_asset(const MapBundle &bundle,
                                             const std::string &path);

} // namespace imrmf::map_editor
