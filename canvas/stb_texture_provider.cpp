// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/stb_texture_provider.hpp"

#include "canvas/texture_decode.hpp"

#include <fstream>

namespace imrmf::map_editor::canvas {

StbTextureProvider::StbTextureProvider(std::filesystem::path root)
    : root_(std::move(root)) {}

namespace {

// Free function so a worker thread can read without holding the provider.
bool read_from(const std::shared_ptr<const AssetBlobs> &blobs,
               const std::filesystem::path &root, const std::string &asset_path,
               std::vector<unsigned char> *out) {
  if (asset_path.empty() || !out)
    return false;

  if (blobs) {
    auto it = blobs->find(asset_path);
    if (it != blobs->end()) {
      *out = it->second;
      return true;
    }
  }

  // No root means the bundle is all there is. Refusing matters, since a bundle
  // from elsewhere could name /etc/anything and Download would pack it up.
  if (root.empty())
    return false;

  // An absolute path is taken at face value, the user's own yaml named it.
  std::filesystem::path p(asset_path);
  if (p.is_relative())
    p = root / p;

  std::ifstream in(p, std::ios::binary);
  if (!in)
    return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return !out->empty();
}

} // namespace

bool StbTextureProvider::read_asset(const std::string &asset_path,
                                    std::vector<unsigned char> *out) const {
  return read_from(blobs_, root_, asset_path, out);
}

void StbTextureProvider::trigger_load(LayerTexture &out,
                                      const std::string &cache_key,
                                      const std::string & /*asset_id*/,
                                      const std::string &asset_path, double tr,
                                      double tg, double tb) {
#ifdef __EMSCRIPTEN__
  std::vector<unsigned char> bytes;
  if (!read_asset(asset_path, &bytes)) {
    out.status = LoadStatus::Failed;
    return;
  }
  decode_into_texture(out, bytes.data(), bytes.size(), tr, tg, tb);
#else
  // By value: the worker outlives this call, and the provider may not.
  loader_.submit(
      cache_key,
      [blobs = blobs_, root = root_,
       asset_path](std::vector<unsigned char> *bytes) {
        return read_from(blobs, root, asset_path, bytes);
      },
      tr, tg, tb);
  (void)out;
#endif
}

#ifndef __EMSCRIPTEN__
void StbTextureProvider::pump() {
  loader_.drain([this](const std::string &key, const DecodedPixels &px) {
    auto it = textures_.find(key);
    if (it != textures_.end())
      upload_decoded(it->second, px);
  });
}
#endif

} // namespace imrmf::map_editor::canvas
