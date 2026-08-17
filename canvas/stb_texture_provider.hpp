// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace imrmf::map_editor::canvas {

// Encoded image bytes keyed by the path as it appears in the building.yaml.
using AssetBlobs = std::map<std::string, std::vector<unsigned char>>;

// The local counterpart to HttpTextureProvider. A yaml off disk sets a root, a
// bundle sets blobs and no root so it never reads the disk. Both is allowed.
class StbTextureProvider : public TextureProvider {
public:
  StbTextureProvider() = default;
  explicit StbTextureProvider(std::filesystem::path root);

  void set_root(std::filesystem::path root) { root_ = std::move(root); }
  const std::filesystem::path &root() const { return root_; }

  void set_blobs(std::shared_ptr<const AssetBlobs> blobs) {
    blobs_ = std::move(blobs);
  }

  bool read_asset(const std::string &asset_path,
                  std::vector<unsigned char> *out) const override;

protected:
  void trigger_load(LayerTexture &out, const std::string &cache_key,
                    const std::string &asset_id, const std::string &asset_path,
                    double tint_r, double tint_g, double tint_b) override;

private:
  std::filesystem::path root_;
  std::shared_ptr<const AssetBlobs> blobs_;
};

} // namespace imrmf::map_editor::canvas
