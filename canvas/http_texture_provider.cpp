// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/http_texture_provider.hpp"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten.h>
#else
#include <GL/gl.h>
#include <curl/curl.h>
#define STB_IMAGE_IMPLEMENTATION
#include "canvas/stb_image.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace imrmf::map_editor::canvas {

namespace {

std::string urlencode(const std::string &s) {
  std::string o;
  o.reserve(s.size() * 3);
  char buf[4];
  for (unsigned char c : s) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (unreserved) {
      o.push_back((char)c);
    } else {
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      o.append(buf);
    }
  }
  return o;
}

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

struct PendingDecode {
  LayerTexture *tex;
  double cr, cg, cb;
};

std::unordered_map<int, PendingDecode> g_pending;
int g_next_handle = 0;

#ifdef __EMSCRIPTEN__
EM_JS(void, imrmf_canvas_fetch, (const char *url_c, int max_dim, int handle), {
  const url = UTF8ToString(url_c);
  (async function() {
    try {
      const r = await fetch(url);
      if (!r.ok) {
        Module._imrmf_canvas_on_layer_decoded(handle, 0, 0, 0, 0, 0, 0);
        return;
      }
      const blob = await r.blob();
      const bitmap = await createImageBitmap(blob);
      const sw = bitmap.width, sh = bitmap.height;
      const ratio = Math.min(1.0, max_dim / Math.max(sw, sh));
      const w = Math.max(1, Math.floor(sw * ratio));
      const h = Math.max(1, Math.floor(sh * ratio));
      const c = new OffscreenCanvas(w, h);
      const ctx = c.getContext('2d');
      ctx.drawImage(bitmap, 0, 0, w, h);
      bitmap.close();
      const img = ctx.getImageData(0, 0, w, h);
      const src = img.data;
      let isColor = false;
      const pxStep = Math.max(1, Math.floor((w * h) / 200)) * 4;
      for (let i = 0; i < src.length; i += pxStep) {
        if (src[i] !== src[i + 1] || src[i + 1] !== src[i + 2]) {
          isColor = true;
          break;
        }
      }
      if (isColor) {
        const ptr = _malloc(w * h * 4);
        HEAPU8.set(src.subarray(0, w * h * 4), ptr);
        Module._imrmf_canvas_on_layer_decoded(handle, ptr, w, h, sw, sh, 1);
      } else {
        const ptr = _malloc(w * h);
        let j = ptr;
        for (let i = 0; i < src.length; i += 4) {
          HEAPU8[j++] = (src[i] + src[i + 1] + src[i + 2]) / 3 | 0;
        }
        Module._imrmf_canvas_on_layer_decoded(handle, ptr, w, h, sw, sh, 0);
      }
    } catch (e) {
      console.error('[imrmf] layer decode failed:', e);
      Module._imrmf_canvas_on_layer_decoded(handle, 0, 0, 0, 0, 0, 0);
    }
  })();
});
#endif

} // namespace

#ifdef __EMSCRIPTEN__
extern "C" EMSCRIPTEN_KEEPALIVE void
imrmf_canvas_on_layer_decoded(int handle, unsigned char *buf, int w, int h,
                              int orig_w, int orig_h, int is_color) {
  auto it = g_pending.find(handle);
  if (it == g_pending.end()) {
    if (buf)
      std::free(buf);
    return;
  }
  PendingDecode pd = it->second;
  g_pending.erase(it);
  if (!buf || w <= 0 || h <= 0) {
    if (buf)
      std::free(buf);
    pd.tex->status = LoadStatus::Failed;
    return;
  }
  pd.tex->width = w;
  pd.tex->height = h;
  pd.tex->orig_width = orig_w > 0 ? orig_w : w;
  pd.tex->orig_height = orig_h > 0 ? orig_h : h;
  pd.tex->is_color = (is_color != 0);
  if (pd.tex->is_color) {
    pd.tex->id = upload_rgba(buf, w, h);
    std::vector<unsigned char> inv((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
      inv[i * 4] = (unsigned char)(255 - buf[i * 4]);
      inv[i * 4 + 1] = (unsigned char)(255 - buf[i * 4 + 1]);
      inv[i * 4 + 2] = (unsigned char)(255 - buf[i * 4 + 2]);
      inv[i * 4 + 3] = buf[i * 4 + 3];
    }
    pd.tex->id_inv = upload_rgba(inv.data(), w, h);
    std::free(buf);
  } else {
    pd.tex->grayscale.assign(buf, buf + (size_t)w * h);
    std::free(buf);
    auto rgba =
        colorize_rgba(pd.tex->grayscale.data(), w, h, pd.cr, pd.cg, pd.cb);
    pd.tex->id = upload_rgba(rgba.data(), w, h);
    auto inv = rgba;
    for (int i = 0; i < w * h; ++i) {
      inv[i * 4] = (unsigned char)(255 - inv[i * 4]);
      inv[i * 4 + 1] = (unsigned char)(255 - inv[i * 4 + 1]);
      inv[i * 4 + 2] = (unsigned char)(255 - inv[i * 4 + 2]);
    }
    pd.tex->id_inv = upload_rgba(inv.data(), w, h);
    pd.tex->last_color_r = pd.cr;
    pd.tex->last_color_g = pd.cg;
    pd.tex->last_color_b = pd.cb;
  }
  pd.tex->status = LoadStatus::Ok;
}
#endif

HttpTextureProvider::HttpTextureProvider()
    : url_builder_([](const std::string &id, const std::string &path) {
        return "/layer_asset?id=" + urlencode(id) + "&path=" + urlencode(path);
      }) {}

HttpTextureProvider::HttpTextureProvider(UrlBuilder b)
    : url_builder_(std::move(b)) {}

#ifndef __EMSCRIPTEN__
namespace {
size_t curl_write_bytes(void *contents, size_t size, size_t nmemb,
                        void *userp) {
  auto *out = static_cast<std::vector<unsigned char> *>(userp);
  size_t total = size * nmemb;
  out->insert(out->end(), static_cast<unsigned char *>(contents),
              static_cast<unsigned char *>(contents) + total);
  return total;
}
} // namespace
#endif

void HttpTextureProvider::trigger_load(LayerTexture &out,
                                       const std::string &asset_id,
                                       const std::string &asset_path, double tr,
                                       double tg, double tb) {
#ifdef __EMSCRIPTEN__
  int handle = ++g_next_handle;
  g_pending[handle] = {&out, tr, tg, tb};
  std::string url = url_builder_(asset_id, asset_path);
  imrmf_canvas_fetch(url.c_str(), 2048, handle);
#else
  std::string url = url_builder_(asset_id, asset_path);
  std::vector<unsigned char> body;
  CURL *curl = curl_easy_init();
  if (!curl) {
    out.status = LoadStatus::Failed;
    return;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_bytes);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  long status = 0;
  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK || status < 200 || status >= 300) {
    out.status = LoadStatus::Failed;
    return;
  }
  int w = 0, h = 0, n = 0;
  unsigned char *rgba = stbi_load_from_memory(
      body.data(), static_cast<int>(body.size()), &w, &h, &n, 4);
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

  bool is_color = false;
  int step = std::max(1, (w * h) / 200);
  for (int i = 0; i < w * h; i += step) {
    if (rgba[i * 4] != rgba[i * 4 + 1] || rgba[i * 4 + 1] != rgba[i * 4 + 2]) {
      is_color = true;
      break;
    }
  }
  out.is_color = is_color;
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
#endif
}

} // namespace imrmf::map_editor::canvas
