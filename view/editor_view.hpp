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
#include <utility>
#include <vector>

namespace imrmf::map_editor {

void set_yjs_readonly(bool v);
bool yjs_readonly();

enum class Mode {
  Pan,
  Vertex,
  Lane,
  Wall,
  Door,
  Measurement,
  Floor,
  Hole,
};

inline bool mode_is_edge(Mode m) {
  return m == Mode::Lane || m == Mode::Wall || m == Mode::Door ||
         m == Mode::Measurement;
}
inline bool mode_is_polygon(Mode m) {
  return m == Mode::Floor || m == Mode::Hole;
}

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
  std::vector<int> selected_walls;
  std::vector<int> selected_doors;
  std::vector<int> selected_measurements;
  int selected_floor = -1;
  int pending_lane_start = -1;
  int pending_edge_start = -1;      // wall / door / measurement chain
  std::vector<int> pending_polygon; // floor / hole loop under construction
  bool dirty = false;

  bool pending_vertex_delete = false;
  int pending_vertex_delete_idx = -1;

  std::string etag;
  SaveState save_state = SaveState::Idle;
  std::string status_message;

  int selected_layer = -1;

  // -1 = no layer currently in direct-map align mode.
  int align_layer_idx = -1;

  // Align floors tool. Inter-level transform is derived from matched-name
  // fiducial pairs; nothing here is stored — the fiducials themselves are.
  bool align_floors_mode = false;
  int align_floors_target = -1;
  int align_floors_next_id = 1;
  std::string align_floors_next_name;
  int align_floors_sel_level = -1;
  int align_floors_sel_idx = -1;
  bool align_floors_placing = false;
  std::string align_floors_image;
  double align_floors_ref_mpp = 0.0;
  double align_floors_tgt_mpp = 0.0;

  std::unordered_map<std::string, FloorplanSession> floorplan_session;
  std::unordered_map<std::string, LayerSession> layer_session;

  bool open_add_layer_modal = false;
  std::string new_layer_name;
  std::string new_layer_filename;

  // Layer rename buffer, kept stable while typing so the old CRDT key is known.
  std::string layer_name_buf;
  int layer_name_buf_idx = -99;

  bool open_layer_browse = false; // request to open the modal
  std::string browse_subdir;      // folder within the building asset dir
  bool browse_relist = false;     // trigger a fresh /assets/list
  std::string browse_status;      // upload / error status line
  std::string browse_last_upload; // name of the last upload we refreshed on

  int pending_commit_vertex = -1;
  int pending_commit_lane = -1;
  std::string pending_commit_layer;
  double pending_commit_time = 0.0;

  bool pending_layer_reorder = false;

  int last_drawn_level_idx = -1;

  bool show_fiducials = false;
  int selected_fiducial_idx = -1;

  bool show_walls = true;
  bool show_floors = true;
  bool show_doors = true;
  bool show_measurements = true;

  std::string active_mutex_group; // selected group, highlighted on canvas
  std::string mutex_rename_buf;   // rename input
  std::string mutex_new_buf;      // new-group input
  bool mutex_adding = false;      // mutex row: adding a new group inline

  struct SnapshotEntry {
    std::string dir;
    std::string sha;
    long long created_at = 0;
  };
  std::vector<SnapshotEntry> snapshots;
  // "" = latest (editable). Any other value = read-only snapshot dir.
  std::string snapshot_dir;
  bool snapshot_request_create = false;
  bool snapshot_request_refresh = false;
  std::string snapshot_request_load;
  std::string snapshot_request_unload;
  std::string snapshot_request_restore;
  std::string snapshot_status;

  std::string branch;
  std::vector<std::string> branches;
  bool branch_request_refresh = false;
  std::string branch_switch_to;
  std::string deploy_request_dir;
  std::string deploy_request_to;
  std::string deploy_latest_to;
  std::string deploy_new_branch;
  std::string deploy_status;
};

struct TopBarHooks {
  std::string connection_label;
  std::string details; // multi-line connection info, for the hover tooltip
  // The same info as label/value pairs, so the modal can lay it out as a form.
  std::vector<std::pair<std::string, std::string>> detail_rows;
  bool can_disconnect = false;
  // Unset hides the button, there being nothing to disconnect from.
  std::function<void()> on_disconnect;
  // Save yaml plus every image as one .rmfmap. Unset hides the button.
  std::function<void()> on_download_map;
  // False for a local yaml or bundle, which hides everything needing a server.
  bool has_server = true;
  // Native only: rewrite the file this map was opened from. Unset hides it.
  std::function<void()> on_save_in_place;
  // Records any edit still pending. Undo rewinds the session's history, so an
  // edit that has not landed in it would be rewound past.
  std::function<void()> on_flush_edits;
  // Undo/redo for a session that keeps its own history. Unset falls back to
  // the CRDT document.
  std::function<bool()> can_undo;
  std::function<bool()> can_redo;
  std::function<void()> on_undo;
  std::function<void()> on_redo;
  bool dirty = false;
  // A server session disconnects, a file session closes.
  std::string disconnect_label = "Disconnect";
};

class EditorView {
public:
  // HttpTextureProvider for a server, StbTextureProvider for disk or a bundle.
  EditorView(std::unique_ptr<canvas::TextureProvider> provider,
             std::string building_id);
  ~EditorView();

  canvas::TextureProvider *texture_provider() {
    return texture_provider_.get();
  }

  void draw(Building &building, EditorState &state,
            const std::function<void()> &save_callback,
            const TopBarHooks &top_bar = {});

  void reset_view() { canvas_.view_state().view_initialized = false; }

  void apply_snapshot_dir(const std::string &dir);

private:
  std::string building_id_;
  std::unique_ptr<canvas::TextureProvider> texture_provider_;
  // Non-null only for the HTTP provider. Snapshot routes mean nothing locally.
  canvas::HttpTextureProvider *http_provider_ = nullptr;
  canvas::MapCanvas canvas_;

  bool marquee_active_ = false;
  ImVec2 marquee_start_{};
  canvas::LayersOverlayState layers_overlay_state_;

  void draw_top_bar(Building &building, EditorState &state,
                    const std::function<void()> &save_callback,
                    const TopBarHooks &top_bar);
  void draw_canvas(Building &building, EditorState &state);
  void draw_building_panel(Building &building, EditorState &state);
  void draw_add_layer_section(Building &building, EditorState &state);
  void draw_layer_config_panel(Building &building, EditorState &state);
  void draw_layer_browse_modal(Building &building, EditorState &state);
  void draw_attribute_panel(Building &building, EditorState &state);
  void draw_mutex_groups_panel(Building &building, EditorState &state);
  void draw_status_bar(const EditorState &state);

  // Direct-map align mode for a single layer. Ctrl = fine/zoom-independent.
  void handle_align_input(Building &building, EditorState &state, bool hovered);

  void draw_align_floors_panel(Building &building, EditorState &state);
  void draw_align_floors_canvas(Building &building, EditorState &state);
  void handle_floor_align_input(Building &building, EditorState &state,
                                bool hovered);
  void draw_version_strip(EditorState &state, const TopBarHooks &top_bar);
};

} // namespace imrmf::map_editor
