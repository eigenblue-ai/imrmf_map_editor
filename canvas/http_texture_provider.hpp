// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"

#include <functional>
#include <string>

namespace imrmf::map_editor::canvas {

// Async image fetch over HTTP from inside wasm. Defaults to the editor server
// route /layer_asset?id=&path=; consumers with a different route override the
// url builder. JS handles fetch + decode + downscale to max 2048 px.
class HttpTextureProvider : public TextureProvider {
public:
  using UrlBuilder = std::function<std::string(const std::string &asset_id,
                                               const std::string &asset_path)>;

  HttpTextureProvider();
  explicit HttpTextureProvider(UrlBuilder url_builder);

protected:
  void trigger_load(LayerTexture &out, const std::string &asset_id,
                    const std::string &asset_path, double tint_r, double tint_g,
                    double tint_b) override;

private:
  UrlBuilder url_builder_;
};

} // namespace imrmf::map_editor::canvas
