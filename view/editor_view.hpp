// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"
#include "canvas/http_texture_provider.hpp"
#include "canvas/overlays.hpp"
#include "model/building.hpp"

#include "imgui/imgui.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace imrmf::map_editor {

enum class Mode {
  Pan,
  Vertex,
  Lane,
};

enum class SaveState {
  Idle,
  Saving,
  Saved,
  Conflict,
  BadRequest,
  NetworkError,
};

using canvas::FloorplanSession;
using canvas::LayerSession;

struct EditorState {
  Mode mode = Mode::Pan;
  int level_idx = 0;
  std::vector<int> selected_vertices;
  std::vector<int> selected_lanes;
  int pending_lane_start = -1;
  bool dirty = false;

  bool pending_vertex_delete = false;
  int pending_vertex_delete_idx = -1;

  std::string etag;
  SaveState save_state = SaveState::Idle;
  std::string status_message;

  int selected_layer = -1;

  std::unordered_map<std::string, FloorplanSession> floorplan_session;
  std::unordered_map<std::string, LayerSession> layer_session;

  bool open_add_layer_modal = false;
  std::string new_layer_name;
  std::string new_layer_filename;

  int pending_commit_vertex = -1;
  int pending_commit_lane = -1;
  std::string pending_commit_layer;
  double pending_commit_time = 0.0;

  bool pending_layer_reorder = false;

  int last_drawn_level_idx = -1;
};

struct TopBarHooks {
  std::string connection_label;
  bool can_disconnect = false;
  std::function<void()> on_disconnect;
};

class EditorView {
public:
  EditorView(std::string image_root_unused, std::string building_id);
  ~EditorView();

  void draw(Building &building, EditorState &state,
            const std::function<void()> &save_callback,
            const TopBarHooks &top_bar = {});

  void reset_view() { canvas_.view_state().view_initialized = false; }

private:
  std::string building_id_;
  std::unique_ptr<canvas::HttpTextureProvider> texture_provider_;
  canvas::MapCanvas canvas_;

  bool marquee_active_ = false;
  ImVec2 marquee_start_{};
  canvas::LayersOverlayState layers_overlay_state_;

  void draw_top_bar(Building &building, EditorState &state,
                    const std::function<void()> &save_callback,
                    const TopBarHooks &top_bar);
  void draw_canvas(Building &building, EditorState &state);
  void draw_add_layer_section(Building &building, EditorState &state);
  void draw_attribute_panel(Building &building, EditorState &state);
  void draw_status_bar(const EditorState &state);
};

} // namespace imrmf::map_editor
