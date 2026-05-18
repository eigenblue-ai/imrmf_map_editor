// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"

#include <filesystem>
#include <string>

namespace imrmf::map_editor::canvas {

// Loads images via stb_image from a root directory on the host filesystem.
class StbTextureProvider : public TextureProvider {
public:
  explicit StbTextureProvider(std::filesystem::path root);

protected:
  void trigger_load(LayerTexture &out, const std::string &asset_id,
                    const std::string &asset_path, double tint_r, double tint_g,
                    double tint_b) override;

private:
  std::filesystem::path root_;
};

} // namespace imrmf::map_editor::canvas
