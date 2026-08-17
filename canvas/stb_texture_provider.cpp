// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/stb_texture_provider.hpp"

#include "canvas/texture_decode.hpp"

#include <fstream>

namespace imrmf::map_editor::canvas {

StbTextureProvider::StbTextureProvider(std::filesystem::path root)
    : root_(std::move(root)) {}

bool StbTextureProvider::read_asset(const std::string &asset_path,
                                    std::vector<unsigned char> *out) const {
  if (asset_path.empty() || !out)
    return false;

  if (blobs_) {
    auto it = blobs_->find(asset_path);
    if (it != blobs_->end()) {
      *out = it->second;
      return true;
    }
  }

  // No root means the bundle is all there is. Refusing matters, since a bundle
  // from elsewhere could name /etc/anything and Download would pack it up.
  if (root_.empty())
    return false;

  // An absolute path is taken at face value, the user's own yaml named it.
  std::filesystem::path p(asset_path);
  if (p.is_relative())
    p = root_ / p;

  std::ifstream in(p, std::ios::binary);
  if (!in)
    return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return !out->empty();
}

void StbTextureProvider::trigger_load(LayerTexture &out,
                                      const std::string & /*cache_key*/,
                                      const std::string & /*asset_id*/,
                                      const std::string &asset_path, double tr,
                                      double tg, double tb) {
  std::vector<unsigned char> bytes;
  if (!read_asset(asset_path, &bytes)) {
    out.status = LoadStatus::Failed;
    return;
  }
  decode_into_texture(out, bytes.data(), bytes.size(), tr, tg, tb);
}

} // namespace imrmf::map_editor::canvas
