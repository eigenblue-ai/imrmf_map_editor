// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"
#ifndef __EMSCRIPTEN__
#include "canvas/texture_loader.hpp"
#endif

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

  void set_url_builder(UrlBuilder b) { url_builder_ = std::move(b); }

  // Prefixed to what the url builder returns. A desktop client has no page
  // origin to resolve against, so it must name the server. Empty keeps the
  // browser behaviour.
  void set_base_url(std::string base);

  // Which building read_asset should ask for. The canvas passes an id per draw
  // call, but a bundle pack has no draw to piggyback on.
  void set_asset_id(std::string id) { asset_id_ = std::move(id); }

#ifndef __EMSCRIPTEN__
  void pump() override;

  // Blocking GET of the same route the textures come from. The browser has JS
  // for this. Without it a desktop client cannot pack a server's images into a
  // bundle, since every other source of bytes is local.
  bool read_asset(const std::string &asset_path,
                  std::vector<unsigned char> *out) const override;
#endif

protected:
  void trigger_load(LayerTexture &out, const std::string &cache_key,
                    const std::string &asset_id, const std::string &asset_path,
                    double tint_r, double tint_g, double tint_b) override;

private:
  UrlBuilder url_builder_;
  std::string base_url_;
  std::string asset_id_;
#ifndef __EMSCRIPTEN__
  AsyncTextureLoader loader_;
#endif
};

} // namespace imrmf::map_editor::canvas
