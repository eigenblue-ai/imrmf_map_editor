// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "imgui/imgui.h"
#include "model/building.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace imrmf::map_editor::canvas {

enum class LoadStatus { NotStarted, Loading, Ok, Failed };

struct LayerTexture {
  LoadStatus status = LoadStatus::NotStarted;
  unsigned int id = 0;
  unsigned int id_inv = 0;
  int width = 0, height = 0;
  int orig_width = 0, orig_height = 0;
  bool is_color = false;
  std::vector<unsigned char> grayscale;
  double last_color_r = -1.0, last_color_g = -1.0, last_color_b = -1.0;
};

class TextureProvider {
public:
  virtual ~TextureProvider();

  LayerTexture &acquire(const std::string &cache_key,
                        const std::string &asset_id,
                        const std::string &asset_path, double tint_r,
                        double tint_g, double tint_b);

  void clear_cache() { textures_.clear(); }

protected:
  virtual void trigger_load(LayerTexture &out, const std::string &asset_id,
                            const std::string &asset_path, double tint_r,
                            double tint_g, double tint_b) = 0;

  std::unordered_map<std::string, LayerTexture> textures_;
};

struct ViewState {
  float scale = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  bool view_initialized = false;
};

struct FloorplanSession {
  bool visible = true;
  float alpha = 1.0f;
  bool invert = false;
};

struct LayerSession {
  bool invert = false;
  std::optional<bool> visible;
  std::optional<float> alpha;
  std::optional<float> color_r, color_g, color_b;
};

ImU32 vertex_color(const Vertex &v);
ImU32 lane_color(const Lane &l);
bool is_bidirectional(const Lane &l);

class MapCanvas;

struct DrawOptions {
  bool draw_floorplan = true;
  bool draw_layers = true;
  bool draw_lanes = true;
  bool draw_vertices = true;
  bool show_vertex_names = true;
  const std::unordered_map<std::string, FloorplanSession> *floorplan_sessions =
      nullptr;
  const std::unordered_map<std::string, LayerSession> *layer_sessions = nullptr;
  std::function<void(const MapCanvas &)> after_draw;
};

class MapCanvas {
public:
  MapCanvas(std::string asset_id, TextureProvider *provider);

  void draw(const Building &building, int level_idx, const DrawOptions &opts);
  void handle_pan_zoom(bool hovered);

  ViewState &view_state() { return view_state_; }
  const ViewState &view_state() const { return view_state_; }

  ImVec2 world_to_screen(double wx, double wy) const;
  std::pair<double, double> screen_to_world(ImVec2 sp) const;

  ImVec2 canvas_pos() const { return canvas_pos_; }
  ImVec2 canvas_size() const { return canvas_size_; }
  ImDrawList *draw_list() const { return draw_list_; }
  const std::string &asset_id() const { return asset_id_; }

private:
  ImVec2 canvas_pos_{}, canvas_size_{}, canvas_center_{};
  ImDrawList *draw_list_ = nullptr;
  std::string asset_id_;
  TextureProvider *provider_;
  ViewState view_state_;
};

} // namespace imrmf::map_editor::canvas
