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

ImU32 mutex_color(const std::string &group) {
  uint32_t h = 2166136261u; // FNV-1a, stable per name
  for (unsigned char c : group) {
    h ^= c;
    h *= 16777619u;
  }
  static const ImU32 palette[] = {
      IM_COL32(232, 104, 104, 235), IM_COL32(104, 168, 232, 235),
      IM_COL32(120, 214, 132, 235), IM_COL32(214, 140, 222, 235),
      IM_COL32(230, 196, 96, 235),  IM_COL32(108, 214, 214, 235),
      IM_COL32(232, 156, 96, 235),  IM_COL32(168, 152, 232, 235),
  };
  return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

bool is_bidirectional(const Lane &l) {
  auto it = l.params.find("bidirectional");
  if (it == l.params.end() || it->second.type != ParamType::BOOL)
    return true;
  return it->second.b;
}

namespace {

std::string mutex_of(const std::map<std::string, ParamValue> &p) {
  auto it = p.find("mutex");
  if (it != p.end() && it->second.type == ParamType::STRING)
    return it->second.s;
  return "";
}

// 0 = unconstrained, +1 = forward (start->end), -1 = backward.
int lane_orientation_sign(const Lane &l) {
  auto it = l.params.find("orientation");
  if (it == l.params.end() || it->second.type != ParamType::STRING)
    return 0;
  if (it->second.s == "forward")
    return 1;
  if (it->second.s == "backward")
    return -1;
  return 0;
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

float poly_signed_area2(const std::vector<ImVec2> &p) {
  float a = 0.0f;
  for (size_t i = 0, n = p.size(); i < n; ++i) {
    const ImVec2 &u = p[i], &v = p[(i + 1) % n];
    a += u.x * v.y - v.x * u.y;
  }
  return a;
}

bool point_in_tri(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c) {
  auto cross = [](ImVec2 u, ImVec2 v, ImVec2 w) {
    return (v.x - u.x) * (w.y - u.y) - (v.y - u.y) * (w.x - u.x);
  };
  float d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
  bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
  bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
  return !(neg && pos);
}

// Ear-clipping fill for a simple polygon. Holes aren't subtracted in v1, they
// get outlined separately instead.
void fill_simple_polygon(ImDrawList *dl, std::vector<ImVec2> pts, ImU32 col) {
  if (pts.size() < 3)
    return;
  if (poly_signed_area2(pts) < 0.0f)
    std::reverse(pts.begin(), pts.end());
  std::vector<int> idx(pts.size());
  for (size_t i = 0; i < pts.size(); ++i)
    idx[i] = (int)i;
  size_t n = idx.size();
  int guard = 0;
  while (n > 3 && guard++ < 20000) {
    bool clipped = false;
    for (size_t i = 0; i < n; ++i) {
      int ia = idx[(i + n - 1) % n], ib = idx[i], ic = idx[(i + 1) % n];
      ImVec2 a = pts[ia], b = pts[ib], c = pts[ic];
      float cr = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (cr <= 0.0f)
        continue;
      bool ear = true;
      for (size_t j = 0; j < n; ++j) {
        int ij = idx[j];
        if (ij == ia || ij == ib || ij == ic)
          continue;
        if (point_in_tri(pts[ij], a, b, c)) {
          ear = false;
          break;
        }
      }
      if (!ear)
        continue;
      dl->AddTriangleFilled(a, b, c, col);
      idx.erase(idx.begin() + i);
      --n;
      clipped = true;
      break;
    }
    if (!clipped)
      break;
  }
  if (n == 3)
    dl->AddTriangleFilled(pts[idx[0]], pts[idx[1]], pts[idx[2]], col);
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
    trigger_load(tex, cache_key, asset_id, asset_path, tr, tg, tb);
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

  // Fit on the first frame. Vertices are available synchronously, so fitting to
  // their bbox lets the first painted frame already be correct instead of
  // snapping once the floorplan image finishes loading. Levels with no vertices
  // fall back to waiting for the floorplan dimensions.
  auto fit_to = [&](double cx, double cy, float bw, float bh) {
    float scale = 1.0f;
    bool have = false;
    if (bw > 1e-3f) {
      scale = canvas_size_.x / bw;
      have = true;
    }
    if (bh > 1e-3f) {
      float s = canvas_size_.y / bh;
      scale = have ? std::min(scale, s) : s;
      have = true;
    }
    scale = have ? scale * 0.9f : 1.0f;
    view_state_.scale = std::clamp(scale, 0.05f, 50.0f);
    view_state_.offset_x = -(float)cx * view_state_.scale;
    view_state_.offset_y = -(float)cy * view_state_.scale;
    view_state_.view_initialized = true;
  };
  if (!view_state_.view_initialized) {
    if (!level.vertices.empty()) {
      double minx = level.vertices[0].x, maxx = minx;
      double miny = level.vertices[0].y, maxy = miny;
      for (const Vertex &v : level.vertices) {
        minx = std::min(minx, v.x);
        maxx = std::max(maxx, v.x);
        miny = std::min(miny, v.y);
        maxy = std::max(maxy, v.y);
      }
      fit_to((minx + maxx) * 0.5, (miny + maxy) * 0.5, (float)(maxx - minx),
             (float)(maxy - miny));
    } else if (opts.draw_floorplan && provider_ &&
               !level.drawing_filename.empty()) {
      LayerTexture &fp = provider_->acquire(
          "fp:" + level.name, asset_id_, level.drawing_filename, 1.0, 1.0, 1.0);
      if (fp.status == LoadStatus::Ok && fp.width > 0 && fp.height > 0)
        fit_to((double)fp.width * 0.5, (double)fp.height * 0.5, (float)fp.width,
               (float)fp.height);
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

  auto vtx_ok = [&](int i) {
    return i >= 0 && i < (int)level.vertices.size();
  };

  if (opts.draw_floors) {
    for (const Floor &f : level.floors) {
      std::vector<ImVec2> pts;
      bool ok = f.vertices.size() >= 3;
      for (int vi : f.vertices) {
        if (!vtx_ok(vi)) {
          ok = false;
          break;
        }
        pts.push_back(world_to_screen(level.vertices[vi].x,
                                      level.vertices[vi].y));
      }
      if (!ok)
        continue;
      fill_simple_polygon(draw_list_, pts, IM_COL32(110, 140, 180, 45));
      draw_list_->AddPolyline(pts.data(), (int)pts.size(),
                              IM_COL32(150, 175, 210, 170), ImDrawFlags_Closed,
                              1.5f);
      for (const auto &hole : f.holes) {
        std::vector<ImVec2> hp;
        bool hok = hole.size() >= 3;
        for (int vi : hole) {
          if (!vtx_ok(vi)) {
            hok = false;
            break;
          }
          hp.push_back(world_to_screen(level.vertices[vi].x,
                                       level.vertices[vi].y));
        }
        if (hok)
          draw_list_->AddPolyline(hp.data(), (int)hp.size(),
                                  IM_COL32(150, 175, 210, 170),
                                  ImDrawFlags_Closed, 1.5f);
      }
    }
  }

  if (opts.draw_walls) {
    for (const Wall &w : level.walls) {
      if (!vtx_ok(w.start_idx) || !vtx_ok(w.end_idx))
        continue;
      ImVec2 a = world_to_screen(level.vertices[w.start_idx].x,
                                 level.vertices[w.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[w.end_idx].x,
                                 level.vertices[w.end_idx].y);
      draw_list_->AddLine(a, b, IM_COL32(70, 130, 220, 235), 3.0f);
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
      float thick = 2.0f;
      if (!opts.highlight_mutex.empty()) {
        if (mutex_of(l.params) == opts.highlight_mutex) {
          col = mutex_color(opts.highlight_mutex);
          thick = 4.0f;
        } else {
          col = IM_COL32(80, 80, 80, 120);
        }
      }
      draw_list_->AddLine(a, b, col, thick);
      ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
      float dx = b.x - a.x, dy = b.y - a.y;
      float len = std::sqrt(dx * dx + dy * dy);
      int orient = lane_orientation_sign(l);
      if (!is_bidirectional(l) && len > 1e-3f) {
        // one-way: hollow double chevron along start->end
        float tx = dx / len, ty = dy / len;
        float px = -ty, py = tx;
        const float s = 5.0f;
        auto chevron = [&](float cx, float cy) {
          ImVec2 tip(cx + tx * s, cy + ty * s);
          ImVec2 l(cx - tx * s + px * s, cy - ty * s + py * s);
          ImVec2 r(cx - tx * s - px * s, cy - ty * s - py * s);
          draw_list_->AddLine(tip, l, col, 2.0f);
          draw_list_->AddLine(tip, r, col, 2.0f);
        };
        chevron(mid.x + tx * 4.0f, mid.y + ty * 4.0f);
        chevron(mid.x - tx * 4.0f, mid.y - ty * 4.0f);
      } else if (orient != 0 && len > 1e-3f) {
        // bidirectional + orientation: filled triangle along the constrained dir
        float ux = dx / len * orient, uy = dy / len * orient;
        float px = -uy, py = ux;
        const float lng = 16.0f, wid = 12.0f;
        ImVec2 tip(mid.x + ux * lng * 0.6f, mid.y + uy * lng * 0.6f);
        ImVec2 lft(mid.x - ux * lng * 0.4f + px * wid * 0.5f,
                   mid.y - uy * lng * 0.4f + py * wid * 0.5f);
        ImVec2 rgt(mid.x - ux * lng * 0.4f - px * wid * 0.5f,
                   mid.y - uy * lng * 0.4f - py * wid * 0.5f);
        draw_list_->AddTriangleFilled(tip, lft, rgt, col);
      }
    }
  }

  if (opts.draw_doors) {
    for (const Door &d : level.doors) {
      if (!vtx_ok(d.start_idx) || !vtx_ok(d.end_idx))
        continue;
      ImVec2 a = world_to_screen(level.vertices[d.start_idx].x,
                                 level.vertices[d.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[d.end_idx].x,
                                 level.vertices[d.end_idx].y);
      const ImU32 col = IM_COL32(235, 150, 40, 240);
      draw_list_->AddLine(a, b, col, 3.0f);
      float len = std::sqrt((b.x - a.x) * (b.x - a.x) +
                            (b.y - a.y) * (b.y - a.y));
      if (len > 1.0f) {
        double deg = 90.0;
        double dir = 1.0;
        auto pd = d.params.find("motion_degrees");
        if (pd != d.params.end())
          deg = pd->second.type == ParamType::INT ? pd->second.i
                                                   : pd->second.d;
        auto pdir = d.params.find("motion_direction");
        if (pdir != d.params.end() && pdir->second.type == ParamType::INT &&
            pdir->second.i < 0)
          dir = -1.0;
        float base = std::atan2(b.y - a.y, b.x - a.x);
        float sweep = (float)(deg * 3.14159265358979323846 / 180.0) *
                      (float)dir;
        draw_list_->PathArcTo(a, len, base, base + sweep, 16);
        draw_list_->PathStroke(IM_COL32(235, 150, 40, 130), 0, 1.5f);
      }
    }
  }

  if (opts.draw_measurements) {
    for (const Measurement &m : level.measurements) {
      if (!vtx_ok(m.start_idx) || !vtx_ok(m.end_idx))
        continue;
      ImVec2 a = world_to_screen(level.vertices[m.start_idx].x,
                                 level.vertices[m.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[m.end_idx].x,
                                 level.vertices[m.end_idx].y);
      const ImU32 col = IM_COL32(255, 225, 110, 235);
      float dx = b.x - a.x, dy = b.y - a.y;
      float len = std::sqrt(dx * dx + dy * dy);
      if (len > 1.0f) {
        dx /= len;
        dy /= len;
        const float dash = 9.0f, gap = 6.0f;
        for (float t = 0.0f; t < len; t += dash + gap) {
          float e = std::min(t + dash, len);
          draw_list_->AddLine(ImVec2(a.x + dx * t, a.y + dy * t),
                              ImVec2(a.x + dx * e, a.y + dy * e), col, 1.8f);
        }
      } else {
        draw_list_->AddLine(a, b, col, 1.8f);
      }
      auto pd = m.params.find("distance");
      if (pd != m.params.end()) {
        double meters = pd->second.type == ParamType::INT ? pd->second.i
                                                          : pd->second.d;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f m", meters);
        draw_list_->AddText(ImVec2((a.x + b.x) * 0.5f + 4.0f,
                                   (a.y + b.y) * 0.5f - 14.0f),
                            col, buf);
      }
    }
  }

  if (opts.draw_vertices) {
    for (const Vertex &v : level.vertices) {
      ImVec2 p = world_to_screen(v.x, v.y);
      ImU32 vc = vertex_color(v);
      float vr = 4.5f;
      if (!opts.highlight_mutex.empty()) {
        if (mutex_of(v.params) == opts.highlight_mutex) {
          vc = mutex_color(opts.highlight_mutex);
          vr = 6.5f;
        } else {
          vc = IM_COL32(80, 80, 80, 140);
        }
      }
      draw_list_->AddCircleFilled(p, vr, vc);
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
