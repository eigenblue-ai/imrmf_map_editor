// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/texture_decode.hpp"

#include "canvas/gl.hpp"

// The only stb_image implementation in the binary. Providers decode through
// decode_into_texture rather than calling stbi_ themselves.
#define STB_IMAGE_IMPLEMENTATION
#include "canvas/stb_image.h"

#include <algorithm>
#include <cmath>

namespace imrmf::map_editor::canvas {

namespace {

void invert_rgb(std::vector<unsigned char> &rgba, int w, int h) {
  for (int i = 0; i < w * h; ++i) {
    rgba[i * 4] = (unsigned char)(255 - rgba[i * 4]);
    rgba[i * 4 + 1] = (unsigned char)(255 - rgba[i * 4 + 1]);
    rgba[i * 4 + 2] = (unsigned char)(255 - rgba[i * 4 + 2]);
  }
}

} // namespace

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
  unsigned char r = (unsigned char)std::round(std::clamp(cr, 0.0, 1.0) * 255.0);
  unsigned char g = (unsigned char)std::round(std::clamp(cg, 0.0, 1.0) * 255.0);
  unsigned char b = (unsigned char)std::round(std::clamp(cb, 0.0, 1.0) * 255.0);
  for (int i = 0; i < w * h; ++i) {
    unsigned char v = gray[(size_t)i];
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

void regenerate_colorize(LayerTexture &tex, double cr, double cg, double cb) {
  if (tex.width <= 0 || tex.height <= 0 || tex.grayscale.empty())
    return;
  auto rgba =
      colorize_rgba(tex.grayscale.data(), tex.width, tex.height, cr, cg, cb);
  if (tex.id)
    glDeleteTextures(1, &tex.id);
  if (tex.id_inv)
    glDeleteTextures(1, &tex.id_inv);
  tex.id = upload_rgba(rgba.data(), tex.width, tex.height);
  auto inv = rgba;
  invert_rgb(inv, tex.width, tex.height);
  tex.id_inv = upload_rgba(inv.data(), tex.width, tex.height);
  tex.last_color_r = cr;
  tex.last_color_g = cg;
  tex.last_color_b = cb;
}

DecodedPixels decode_pixels(const unsigned char *bytes, std::size_t len,
                            double tr, double tg, double tb) {
  DecodedPixels px;
  int w = 0, h = 0, n = 0;
  unsigned char *rgba =
      bytes && len ? stbi_load_from_memory(bytes, (int)len, &w, &h, &n, 4)
                   : nullptr;
  if (!rgba || w <= 0 || h <= 0) {
    if (rgba)
      stbi_image_free(rgba);
    return px;
  }
  px.width = w;
  px.height = h;
  px.is_color = detect_color(rgba, w, h);

  if (px.is_color) {
    px.rgba.assign(rgba, rgba + (size_t)w * h * 4);
  } else {
    px.grayscale.assign((size_t)w * h, 0);
    for (int i = 0; i < w * h; ++i) {
      px.grayscale[i] =
          (unsigned char)((rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2]) /
                          3);
    }
    px.rgba = colorize_rgba(px.grayscale.data(), w, h, tr, tg, tb);
  }
  stbi_image_free(rgba);

  px.rgba_inv = px.rgba;
  invert_rgb(px.rgba_inv, w, h);
  px.ok = true;
  return px;
}

void upload_decoded(LayerTexture &out, const DecodedPixels &px) {
  if (!px.ok) {
    out.status = LoadStatus::Failed;
    return;
  }
  out.width = px.width;
  out.height = px.height;
  out.orig_width = px.width;
  out.orig_height = px.height;
  out.is_color = px.is_color;
  out.grayscale = px.grayscale;
  out.id = upload_rgba(px.rgba.data(), px.width, px.height);
  out.id_inv = upload_rgba(px.rgba_inv.data(), px.width, px.height);
  out.status = LoadStatus::Ok;
}

void decode_into_texture(LayerTexture &out, const unsigned char *bytes,
                         std::size_t len, double tr, double tg, double tb) {
  const DecodedPixels px = decode_pixels(bytes, len, tr, tg, tb);
  upload_decoded(out, px);
  if (px.ok && !px.is_color) {
    out.last_color_r = tr;
    out.last_color_g = tg;
    out.last_color_b = tb;
  }
}

} // namespace imrmf::map_editor::canvas
