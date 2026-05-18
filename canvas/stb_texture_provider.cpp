// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/stb_texture_provider.hpp"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "canvas/stb_image.h"

#include <vector>

namespace imrmf::map_editor::canvas {

namespace {

unsigned int upload_rgba(const unsigned char *data, int w, int h) {
  unsigned int id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               data);
  return id;
}

std::vector<unsigned char> colorize_rgba(const unsigned char *gray, int w,
                                         int h, double cr, double cg,
                                         double cb) {
  std::vector<unsigned char> out((size_t)w * h * 4);
  unsigned char r = (unsigned char)((cr < 0 ? 0 : cr > 1 ? 1 : cr) * 255.0);
  unsigned char g = (unsigned char)((cg < 0 ? 0 : cg > 1 ? 1 : cg) * 255.0);
  unsigned char b = (unsigned char)((cb < 0 ? 0 : cb > 1 ? 1 : cb) * 255.0);
  for (int i = 0; i < w * h; ++i) {
    unsigned char v = gray[i];
    if (v < 100) {
      out[i * 4] = r;
      out[i * 4 + 1] = g;
      out[i * 4 + 2] = b;
      out[i * 4 + 3] = 127;
    } else if (v > 200) {
      out[i * 4] = 0;
      out[i * 4 + 1] = 0;
      out[i * 4 + 2] = 0;
      out[i * 4 + 3] = 0;
    } else {
      out[i * 4] = v;
      out[i * 4 + 1] = v;
      out[i * 4 + 2] = v;
      out[i * 4 + 3] = 50;
    }
  }
  return out;
}

bool detect_color(const unsigned char *rgba, int w, int h) {
  int step = std::max(1, (w * h) / 200);
  for (int i = 0; i < w * h; i += step) {
    if (rgba[i * 4] != rgba[i * 4 + 1] || rgba[i * 4 + 1] != rgba[i * 4 + 2])
      return true;
  }
  return false;
}

} // namespace

StbTextureProvider::StbTextureProvider(std::filesystem::path root)
    : root_(std::move(root)) {}

void StbTextureProvider::trigger_load(LayerTexture &out,
                                      const std::string &asset_id,
                                      const std::string &asset_path, double tr,
                                      double tg, double tb) {
  std::filesystem::path full = root_ / asset_id / asset_path;
  int w = 0, h = 0, n = 0;
  unsigned char *rgba = stbi_load(full.string().c_str(), &w, &h, &n, 4);
  if (!rgba || w <= 0 || h <= 0) {
    if (rgba)
      stbi_image_free(rgba);
    out.status = LoadStatus::Failed;
    return;
  }
  out.width = w;
  out.height = h;
  out.orig_width = w;
  out.orig_height = h;
  out.is_color = detect_color(rgba, w, h);

  if (out.is_color) {
    out.id = upload_rgba(rgba, w, h);
    std::vector<unsigned char> inv((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
      inv[i * 4] = (unsigned char)(255 - rgba[i * 4]);
      inv[i * 4 + 1] = (unsigned char)(255 - rgba[i * 4 + 1]);
      inv[i * 4 + 2] = (unsigned char)(255 - rgba[i * 4 + 2]);
      inv[i * 4 + 3] = rgba[i * 4 + 3];
    }
    out.id_inv = upload_rgba(inv.data(), w, h);
  } else {
    out.grayscale.assign((size_t)w * h, 0);
    for (int i = 0; i < w * h; ++i) {
      out.grayscale[i] =
          (unsigned char)((rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2]) /
                          3);
    }
    auto colored = colorize_rgba(out.grayscale.data(), w, h, tr, tg, tb);
    out.id = upload_rgba(colored.data(), w, h);
    auto inv = colored;
    for (int i = 0; i < w * h; ++i) {
      inv[i * 4] = (unsigned char)(255 - inv[i * 4]);
      inv[i * 4 + 1] = (unsigned char)(255 - inv[i * 4 + 1]);
      inv[i * 4 + 2] = (unsigned char)(255 - inv[i * 4 + 2]);
    }
    out.id_inv = upload_rgba(inv.data(), w, h);
    out.last_color_r = tr;
    out.last_color_g = tg;
    out.last_color_b = tb;
  }
  stbi_image_free(rgba);
  out.status = LoadStatus::Ok;
}

} // namespace imrmf::map_editor::canvas
