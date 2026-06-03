// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/canvas.hpp"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace imrmf::map_editor::canvas {

ImU32 vertex_color(const Vertex &v) {
  auto flag = [&](const char *k) {
    auto it = v.params.find(k);
    return it != v.params.end() && it->second.type == ParamType::BOOL &&
           it->second.b;
  };
  auto has_str = [&](const char *k) {
    auto it = v.params.find(k);
    return it != v.params.end() && it->second.type == ParamType::STRING &&
           !it->second.s.empty();
  };
  if (flag("is_charger"))
    return IM_COL32(220, 60, 60, 255);
  if (flag("is_parking_spot"))
    return IM_COL32(80, 140, 255, 255);
  if (has_str("pickup_dispenser"))
    return IM_COL32(60, 200, 80, 255);
  if (has_str("dropoff_ingestor"))
    return IM_COL32(255, 140, 40, 255);
  if (flag("is_holding_point"))
    return IM_COL32(230, 220, 60, 255);
  if (flag("is_passthrough_point"))
    return IM_COL32(170, 170, 170, 255);
  return IM_COL32(230, 230, 230, 255);
}

ImU32 lane_color(const Lane &l) {
  auto it = l.params.find("graph_idx");
  int g = (it != l.params.end() && it->second.type == ParamType::INT)
              ? it->second.i
              : 0;
  static const ImU32 palette[] = {
      IM_COL32(120, 170, 255, 220), IM_COL32(255, 160, 120, 220),
      IM_COL32(140, 220, 140, 220), IM_COL32(220, 140, 220, 220),
      IM_COL32(220, 220, 120, 220),
  };
  return palette[((g % 5) + 5) % 5];
}

bool is_bidirectional(const Lane &l) {
  auto it = l.params.find("bidirectional");
  if (it == l.params.end() || it->second.type != ParamType::BOOL)
    return true;
  return it->second.b;
}

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

std::vector<unsigned char> colorize_rgba(const std::vector<unsigned char> &gray,
                                         int w, int h, double cr, double cg,
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

void regenerate_colorize(LayerTexture &tex, double cr, double cg, double cb) {
  if (tex.width <= 0 || tex.height <= 0 || tex.grayscale.empty())
    return;
  auto rgba = colorize_rgba(tex.grayscale, tex.width, tex.height, cr, cg, cb);
  if (tex.id)
    glDeleteTextures(1, &tex.id);
  if (tex.id_inv)
    glDeleteTextures(1, &tex.id_inv);
  tex.id = upload_rgba(rgba.data(), tex.width, tex.height);
  auto inv = rgba;
  for (int i = 0; i < tex.width * tex.height; ++i) {
    inv[i * 4] = (unsigned char)(255 - inv[i * 4]);
    inv[i * 4 + 1] = (unsigned char)(255 - inv[i * 4 + 1]);
    inv[i * 4 + 2] = (unsigned char)(255 - inv[i * 4 + 2]);
  }
  tex.id_inv = upload_rgba(inv.data(), tex.width, tex.height);
  tex.last_color_r = cr;
  tex.last_color_g = cg;
  tex.last_color_b = cb;
}

} // namespace

TextureProvider::~TextureProvider() {
  for (auto &[_, t] : textures_) {
    if (t.id)
      glDeleteTextures(1, &t.id);
    if (t.id_inv)
      glDeleteTextures(1, &t.id_inv);
  }
}

LayerTexture &TextureProvider::acquire(const std::string &cache_key,
                                       const std::string &asset_id,
                                       const std::string &asset_path, double tr,
                                       double tg, double tb) {
  auto &tex = textures_[cache_key];
  if (tex.status == LoadStatus::NotStarted) {
    tex.status = LoadStatus::Loading;
    trigger_load(tex, asset_id, asset_path, tr, tg, tb);
  }
  return tex;
}

MapCanvas::MapCanvas(std::string asset_id, TextureProvider *provider)
    : asset_id_(std::move(asset_id)), provider_(provider) {}

ImVec2 MapCanvas::world_to_screen(double wx, double wy) const {
  return view_world_to_screen(view_state_, canvas_center_, wx, wy);
}

std::pair<double, double> MapCanvas::screen_to_world(ImVec2 sp) const {
  return view_screen_to_world(view_state_, canvas_center_, sp);
}

void MapCanvas::draw(const Building &building, int level_idx,
                     const DrawOptions &opts) {
  if (building.levels.empty()) {
    ImGui::Text("No levels.");
    return;
  }
  level_idx = std::max(0, std::min(level_idx, (int)building.levels.size() - 1));
  const Level &level = building.levels[level_idx];
  double mpp = compute_level_mpp(building, level_idx);
  double eff_mpp = mpp > 0.0 ? mpp : 1.0;

  canvas_pos_ = ImGui::GetCursorScreenPos();
  canvas_size_ = ImGui::GetContentRegionAvail();
  if (canvas_size_.x < 50)
    canvas_size_.x = 50;
  if (canvas_size_.y < 50)
    canvas_size_.y = 50;
  ImVec2 canvas_end(canvas_pos_.x + canvas_size_.x,
                    canvas_pos_.y + canvas_size_.y);
  canvas_center_ = ImVec2(canvas_pos_.x + canvas_size_.x * 0.5f,
                          canvas_pos_.y + canvas_size_.y * 0.5f);

  // Fit-to-canvas on first frame using the floorplan dimensions.
  if (!view_state_.view_initialized && opts.draw_floorplan && provider_ &&
      !level.drawing_filename.empty()) {
    LayerTexture &fp = provider_->acquire(
        "fp:" + level.name, asset_id_, level.drawing_filename, 1.0, 1.0, 1.0);
    if (fp.status == LoadStatus::Ok && fp.width > 0 && fp.height > 0) {
      float sx = canvas_size_.x / (float)fp.width;
      float sy = canvas_size_.y / (float)fp.height;
      view_state_.scale = std::min(sx, sy) * 0.9f;
      view_state_.offset_x = -(float)fp.width * 0.5f * view_state_.scale;
      view_state_.offset_y = -(float)fp.height * 0.5f * view_state_.scale;
      view_state_.view_initialized = true;
    }
  }

  draw_list_ = ImGui::GetWindowDrawList();
  draw_list_->PushClipRect(canvas_pos_, canvas_end, true);
  draw_list_->AddRectFilled(canvas_pos_, canvas_end, IM_COL32(30, 30, 35, 255));

  FloorplanSession fp_sess;
  if (opts.floorplan_sessions) {
    auto it = opts.floorplan_sessions->find(level.name);
    if (it != opts.floorplan_sessions->end())
      fp_sess = it->second;
  }
  if (opts.draw_floorplan && fp_sess.visible && provider_ &&
      !level.drawing_filename.empty()) {
    LayerTexture &fp = provider_->acquire(
        "fp:" + level.name, asset_id_, level.drawing_filename, 1.0, 1.0, 1.0);
    if (fp.status == LoadStatus::Ok) {
      ImVec2 p_min = world_to_screen(0.0, 0.0);
      ImVec2 p_max = world_to_screen((double)fp.width, (double)fp.height);
      int a255 =
          (int)std::round(std::clamp(fp_sess.alpha, 0.0f, 1.0f) * 255.0f);
      unsigned int id = fp_sess.invert ? fp.id_inv : fp.id;
      draw_list_->AddImage((void *)(intptr_t)id, p_min, p_max, ImVec2(0, 0),
                           ImVec2(1, 1), IM_COL32(255, 255, 255, a255));
    }
  }

  // Layers render back to front (level.layers[0] sits on top).
  if (opts.draw_layers && provider_) {
    for (int i = (int)level.layers.size() - 1; i >= 0; --i) {
      const Layer &L = level.layers[i];
      const LayerSession *sess = nullptr;
      if (opts.layer_sessions) {
        auto it = opts.layer_sessions->find(level.name + ":" + L.name);
        if (it != opts.layer_sessions->end())
          sess = &it->second;
      }
      bool visible =
          sess && sess->visible.has_value() ? *sess->visible : L.visible;
      if (!visible)
        continue;
      double cr = sess && sess->color_r ? (double)*sess->color_r : L.color_r;
      double cg = sess && sess->color_g ? (double)*sess->color_g : L.color_g;
      double cb = sess && sess->color_b ? (double)*sess->color_b : L.color_b;
      double ca = sess && sess->alpha ? (double)*sess->alpha : L.color_a;
      std::string key = "lay:" + level.name + ":" + L.name;
      LayerTexture &tex =
          provider_->acquire(key, asset_id_, L.filename, cr, cg, cb);
      if (tex.status != LoadStatus::Ok)
        continue;
      if (!tex.is_color && (tex.last_color_r != cr || tex.last_color_g != cg ||
                            tex.last_color_b != cb)) {
        regenerate_colorize(tex, cr, cg, cb);
      }
      // Size the world rect by orig dims so downscaled textures still cover
      // the same physical region.
      double s_w = L.scale / eff_mpp;
      double tx = L.translation_x / eff_mpp;
      double ty = L.translation_y / eff_mpp;
      double cy = std::cos(L.yaw), sy = std::sin(L.yaw);
      auto i2w = [&](double ix, double iy) {
        double a = ix * s_w, b = iy * s_w;
        return std::pair<double, double>(tx + cy * a - sy * b,
                                         ty + sy * a + cy * b);
      };
      int ow = tex.orig_width > 0 ? tex.orig_width : tex.width;
      int oh = tex.orig_height > 0 ? tex.orig_height : tex.height;
      auto [w0x, w0y] = i2w(0.0, 0.0);
      auto [w1x, w1y] = i2w((double)ow, 0.0);
      auto [w2x, w2y] = i2w((double)ow, (double)oh);
      auto [w3x, w3y] = i2w(0.0, (double)oh);
      int a255 = (int)std::round(std::clamp(ca, 0.0, 1.0) * 255.0);
      bool invert = sess && sess->invert;
      unsigned int id = invert ? tex.id_inv : tex.id;
      draw_list_->AddImageQuad(
          (void *)(intptr_t)id, world_to_screen(w0x, w0y),
          world_to_screen(w1x, w1y), world_to_screen(w2x, w2y),
          world_to_screen(w3x, w3y), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
          ImVec2(0, 1), IM_COL32(255, 255, 255, a255));
    }
  }

  if (opts.draw_lanes) {
    for (const Lane &l : level.lanes) {
      if (l.start_idx < 0 || l.start_idx >= (int)level.vertices.size() ||
          l.end_idx < 0 || l.end_idx >= (int)level.vertices.size())
        continue;
      ImVec2 a = world_to_screen(level.vertices[l.start_idx].x,
                                 level.vertices[l.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[l.end_idx].x,
                                 level.vertices[l.end_idx].y);
      ImU32 col = lane_color(l);
      draw_list_->AddLine(a, b, col, 2.0f);
      if (!is_bidirectional(l)) {
        ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1e-3f) {
          dx /= len;
          dy /= len;
          const float h = 8.0f;
          ImVec2 tip(mid.x + dx * h, mid.y + dy * h);
          ImVec2 lft(mid.x - dy * h * 0.5f, mid.y + dx * h * 0.5f);
          ImVec2 rgt(mid.x + dy * h * 0.5f, mid.y - dx * h * 0.5f);
          draw_list_->AddTriangleFilled(tip, lft, rgt, col);
        }
      }
    }
  }

  if (opts.draw_vertices) {
    for (const Vertex &v : level.vertices) {
      ImVec2 p = world_to_screen(v.x, v.y);
      draw_list_->AddCircleFilled(p, 4.5f, vertex_color(v));
      if (opts.show_vertex_names && !v.name.empty()) {
        draw_list_->AddText(ImVec2(p.x + 6.0f, p.y - 8.0f),
                            IM_COL32(220, 220, 220, 255), v.name.c_str());
      }
    }
  }

  if (opts.after_draw)
    opts.after_draw(*this);

  draw_list_->PopClipRect();
}

void MapCanvas::handle_pan_zoom(bool hovered) {
  ::imrmf::map_editor::canvas::handle_pan_zoom(view_state_, canvas_center_,
                                               hovered);
}

} // namespace imrmf::map_editor::canvas
