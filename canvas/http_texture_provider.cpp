// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/http_texture_provider.hpp"

#include "canvas/gl.hpp"
#include "canvas/texture_decode.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <curl/curl.h>
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

#ifdef __EMSCRIPTEN__
// Empty texture for JS to fill later via texImage2D on the GL thread.
unsigned int alloc_gl_texture() {
  unsigned int id = 0;
  glGenTextures(1, &id);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glBindTexture(GL_TEXTURE_2D, 0);
  return id;
}
#endif

struct PendingDecode {
  LayerTexture *tex;
  double cr, cg, cb;
};

std::unordered_map<int, PendingDecode> g_worker_pending;
int g_next_handle = 0;

#ifdef __EMSCRIPTEN__
// Layer path: a Web Worker does the fetch, decode, colorize and invert so none
// of that per-pixel work runs on the render thread. The worker posts back the
// finished RGBA buffers (plus the grayscale source for later recolor) and the
// main thread does only the cheap GL upload.
EM_JS(void, imrmf_canvas_fetch_worker,
      (const char *url_c, int max_dim, int handle, int gl_id, int gl_id_inv,
       double r, double g, double b), {
        try {
          if (!Module._imrmfLayerWorker) {
            const workerSrc = `
self.onmessage = async (e) => {
  const m = e.data;
  const fail = () => self.postMessage({handle: m.handle, ok: false});
  try {
    const resp = await fetch(m.url);
    if (!resp.ok) return fail();
    const bm = await createImageBitmap(await resp.blob());
    const sw = bm.width, sh = bm.height;
    const ratio = Math.min(1, m.maxDim / Math.max(sw, sh));
    const w = Math.max(1, Math.round(sw*ratio)), h = Math.max(1, Math.round(sh*ratio));
    const cv = new OffscreenCanvas(w, h);
    const cx = cv.getContext('2d');
    cx.drawImage(bm, 0, 0, w, h);
    bm.close();
    const src = cx.getImageData(0, 0, w, h).data;
    let isColor = false;
    const step = Math.max(1, Math.floor((w*h)/200))*4;
    for (let i=0;i<src.length;i+=step){ if(src[i]!==src[i+1]||src[i+1]!==src[i+2]){isColor=true;break;} }
    if (isColor) {
      const rgba = new Uint8Array(src);
      const inv = rgba.slice(0);
      for (let i=0;i<w*h;i++){ inv[i*4]=255-inv[i*4]; inv[i*4+1]=255-inv[i*4+1]; inv[i*4+2]=255-inv[i*4+2]; }
      self.postMessage({handle:m.handle, ok:true, w, h, sw, sh, isColor:true, rgba:rgba.buffer, inv:inv.buffer}, [rgba.buffer, inv.buffer]);
    } else {
      const gray = new Uint8Array(w*h);
      for (let i=0;i<w*h;i++){ gray[i]=(src[i*4]+src[i*4+1]+src[i*4+2])/3|0; }
      const R=Math.max(0,Math.min(1,m.r))*255|0, G=Math.max(0,Math.min(1,m.g))*255|0, B=Math.max(0,Math.min(1,m.b))*255|0;
      const col = new Uint8Array(w*h*4);
      for (let i=0;i<w*h;i++){ const v=gray[i]; if(v<100){col[i*4]=R;col[i*4+1]=G;col[i*4+2]=B;col[i*4+3]=127;} else if(v>200){col[i*4+3]=0;} else {col[i*4]=v;col[i*4+1]=v;col[i*4+2]=v;col[i*4+3]=50;} }
      const inv = col.slice(0);
      for (let i=0;i<w*h;i++){ inv[i*4]=255-inv[i*4]; inv[i*4+1]=255-inv[i*4+1]; inv[i*4+2]=255-inv[i*4+2]; }
      self.postMessage({handle:m.handle, ok:true, w, h, sw, sh, isColor:false, gray:gray.buffer, rgba:col.buffer, inv:inv.buffer}, [gray.buffer, col.buffer, inv.buffer]);
    }
  } catch (err) { console.error('[imrmf] worker decode failed:', err); fail(); }
};`;
            const blobUrl = URL.createObjectURL(
                new Blob([workerSrc], {type : 'text/javascript'}));
            const wk = new Worker(blobUrl);
            Module._imrmfLayerPending = {};
            wk.onmessage = (e) => {
              const d = e.data;
              const pend = Module._imrmfLayerPending[d.handle];
              delete Module._imrmfLayerPending[d.handle];
              if (!d.ok || !pend) {
                Module._imrmf_canvas_on_worker_decoded(d.handle, 0, 0, 0, 0, 0,
                                                       0, 0);
                return;
              }
              const gl = (typeof GL !== 'undefined' && GL.currentContext)
                             ? GL.currentContext.GLctx
                             : (Module.GLctx || Module.ctx);
              const upload = (glId, buf) => {
                if (!gl || !buf) return;
                const tex = (typeof GL !== 'undefined' && GL.textures)
                                ? GL.textures[glId]
                                : null;
                if (!tex) return;
                const prev = gl.getParameter(gl.TEXTURE_BINDING_2D);
                gl.bindTexture(gl.TEXTURE_2D, tex);
                gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
                gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
                gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, d.w, d.h, 0, gl.RGBA,
                              gl.UNSIGNED_BYTE, new Uint8Array(buf));
                gl.bindTexture(gl.TEXTURE_2D, prev || null);
              };
              upload(pend.id, d.rgba);
              upload(pend.idInv, d.inv);
              let grayPtr = 0, grayLen = 0;
              if (d.gray) {
                grayLen = d.gray.byteLength;
                grayPtr = _malloc(grayLen);
                HEAPU8.set(new Uint8Array(d.gray), grayPtr);
              }
              Module._imrmf_canvas_on_worker_decoded(d.handle, d.w, d.h, d.sw,
                                                     d.sh, d.isColor ? 1 : 0,
                                                     grayPtr, grayLen);
            };
            Module._imrmfLayerWorker = wk;
          }
          Module._imrmfLayerPending[handle] = {id : gl_id, idInv : gl_id_inv};
          // Resolve to absolute here, in the page context. A root-relative URL
          // would otherwise resolve against the worker's blob: base and fail.
          const absUrl = new URL(UTF8ToString(url_c), self.location.href).href;
          Module._imrmfLayerWorker.postMessage({
            handle : handle,
            url : absUrl,
            maxDim : max_dim,
            r : r,
            g : g,
            b : b
          });
        } catch (e) {
          console.error('[imrmf] layer worker failed:', e);
          Module._imrmf_canvas_on_worker_decoded(handle, 0, 0, 0, 0, 0, 0, 0);
        }
      });

// Fast path: decode and upload straight into a GL texture, skipping the CPU
// readback + per-pixel loops that block the main thread. Used for the
// floorplan, which is never recolored.
EM_JS(void, imrmf_canvas_fetch_fast,
      (const char *url_c, int max_dim, int handle, int gl_id, int gl_id_inv), {
        const url = UTF8ToString(url_c);
        (async function() {
          const fail = () =>
              Module._imrmf_canvas_on_fast_decoded(handle, 0, 0, 0, 0);
          try {
            const r = await fetch(url);
            if (!r.ok) { fail(); return; }
            const blob = await r.blob();
            const bitmap = await createImageBitmap(blob);
            const sw = bitmap.width, sh = bitmap.height;
            const ratio = Math.min(1.0, max_dim / Math.max(sw, sh));
            const w = Math.max(1, Math.round(sw * ratio));
            const h = Math.max(1, Math.round(sh * ratio));
            const gl = (typeof GL !== 'undefined' && GL.currentContext)
                           ? GL.currentContext.GLctx
                           : (Module.GLctx || Module.ctx);
            const tex = (typeof GL !== 'undefined' && GL.textures)
                            ? GL.textures[gl_id]
                            : null;
            if (!gl || !tex) { bitmap.close(); fail(); return; }
            let src = bitmap;
            if (ratio < 1.0) {
              const c = new OffscreenCanvas(w, h);
              c.getContext('2d').drawImage(bitmap, 0, 0, w, h);
              src = c;
            }
            const prev = gl.getParameter(gl.TEXTURE_BINDING_2D);
            gl.bindTexture(gl.TEXTURE_2D, tex);
            gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
            gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE,
                          src);
            const texInv = (gl_id_inv && typeof GL !== 'undefined' && GL.textures)
                               ? GL.textures[gl_id_inv]
                               : null;
            if (texInv) {
              const ic = new OffscreenCanvas(w, h);
              const icx = ic.getContext('2d');
              icx.filter = 'invert(1)';
              icx.drawImage(src, 0, 0, w, h);
              gl.bindTexture(gl.TEXTURE_2D, texInv);
              gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE,
                            ic);
            }
            gl.bindTexture(gl.TEXTURE_2D, prev || null);
            bitmap.close();
            Module._imrmf_canvas_on_fast_decoded(handle, w, h, sw, sh);
          } catch (e) {
            console.error('[imrmf] fast decode failed:', e);
            fail();
          }
        })();
      });
#endif

std::unordered_map<int, LayerTexture *> g_fast_pending;

} // namespace

#ifdef __EMSCRIPTEN__
// Worker already uploaded the textures into out.id / out.id_inv. We only record
// the dimensions, keep the grayscale source for later recolor, and flip status.
extern "C" EMSCRIPTEN_KEEPALIVE void
imrmf_canvas_on_worker_decoded(int handle, int w, int h, int orig_w, int orig_h,
                               int is_color, unsigned char *gray,
                               int gray_len) {
  auto it = g_worker_pending.find(handle);
  if (it == g_worker_pending.end()) {
    if (gray)
      std::free(gray);
    return;
  }
  PendingDecode pd = it->second;
  g_worker_pending.erase(it);
  if (w <= 0 || h <= 0) {
    if (gray)
      std::free(gray);
    pd.tex->status = LoadStatus::Failed;
    return;
  }
  pd.tex->width = w;
  pd.tex->height = h;
  pd.tex->orig_width = orig_w > 0 ? orig_w : w;
  pd.tex->orig_height = orig_h > 0 ? orig_h : h;
  pd.tex->is_color = (is_color != 0);
  if (!pd.tex->is_color && gray && gray_len > 0) {
    pd.tex->grayscale.assign(gray, gray + gray_len);
    pd.tex->last_color_r = pd.cr;
    pd.tex->last_color_g = pd.cg;
    pd.tex->last_color_b = pd.cb;
  }
  if (gray)
    std::free(gray);
  pd.tex->status = LoadStatus::Ok;
}

extern "C" EMSCRIPTEN_KEEPALIVE void
imrmf_canvas_on_fast_decoded(int handle, int w, int h, int orig_w, int orig_h) {
  auto it = g_fast_pending.find(handle);
  if (it == g_fast_pending.end())
    return;
  LayerTexture *tex = it->second;
  g_fast_pending.erase(it);
  if (w <= 0 || h <= 0) {
    tex->status = LoadStatus::Failed;
    return;
  }
  tex->width = w;
  tex->height = h;
  tex->orig_width = orig_w > 0 ? orig_w : w;
  tex->orig_height = orig_h > 0 ? orig_h : h;
  tex->is_color = true;
  tex->status = LoadStatus::Ok;
}
#endif

HttpTextureProvider::HttpTextureProvider()
    : url_builder_([](const std::string &id, const std::string &path) {
        return "/layer_asset?id=" + urlencode(id) + "&path=" + urlencode(path);
      }) {}

HttpTextureProvider::HttpTextureProvider(UrlBuilder b)
    : url_builder_(std::move(b)) {}

void HttpTextureProvider::set_base_url(std::string base) {
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  base_url_ = std::move(base);
}

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
                                       const std::string &cache_key,
                                       const std::string &asset_id,
                                       const std::string &asset_path, double tr,
                                       double tg, double tb) {
#ifdef __EMSCRIPTEN__
  std::string url = base_url_ + url_builder_(asset_id, asset_path);
  // Floorplan is display-only (never recolored), so take the GPU fast path that
  // skips the CPU readback + pixel loops that freeze the UI on load.
  if (cache_key.rfind("fp:", 0) == 0) {
    out.id = alloc_gl_texture();
    out.id_inv = alloc_gl_texture();
    int handle = ++g_next_handle;
    g_fast_pending[handle] = &out;
    imrmf_canvas_fetch_fast(url.c_str(), 2048, handle, (int)out.id,
                            (int)out.id_inv);
    return;
  }
  out.id = alloc_gl_texture();
  out.id_inv = alloc_gl_texture();
  int handle = ++g_next_handle;
  g_worker_pending[handle] = {&out, tr, tg, tb};
  imrmf_canvas_fetch_worker(url.c_str(), 2048, handle, (int)out.id,
                            (int)out.id_inv, tr, tg, tb);
#else
  std::string url = base_url_ + url_builder_(asset_id, asset_path);
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
  decode_into_texture(out, body.data(), body.size(), tr, tg, tb);
#endif
}

} // namespace imrmf::map_editor::canvas
