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
//   layers/<level>/x.png   every image at the path the yaml names it by
//
// That is the shape a backend keeps a building in, so a bundle unzips into a
// folder identical to the bucket's <id>/ (minus snapshots), and a zipped
// building folder opens as a bundle even with no manifest.json in it.
//
// Version 1 put images under assets/. Those still open, since the manifest
// records where each one lives.

inline constexpr const char *kBundleExtension = ".rmfmap";
inline constexpr const char *kBundleFormat = "rmfmap";
inline constexpr int kBundleVersion = 2;

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
