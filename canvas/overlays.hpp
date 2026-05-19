// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "canvas/canvas.hpp"
#include "model/building.hpp"

#include <functional>
#include <string>
#include <unordered_map>

namespace imrmf::map_editor::canvas {

struct LayersOverlayState {
  bool expanded = false;
  int selected_layer = -1;
  bool pending_reorder_commit = false;
};

// Editor wires these to its CRDT writers
// Runner leaves them empty so edits stay session-only.
struct LayerEditCallbacks {
  std::function<void(const Layer &updated)> on_layer_commit;
  std::function<void(const std::string &layer_name)> on_layer_delete;
  std::function<void(const std::vector<std::string> &new_order)>
      on_layer_reorder;
};

bool draw_level_selector_overlay(const Building &building, int &level_idx,
                                 const MapCanvas &canvas);

void draw_layers_overlay(
    Building &building, int level_idx,
    std::unordered_map<std::string, FloorplanSession> &fp_sessions,
    std::unordered_map<std::string, LayerSession> &layer_sessions,
    LayersOverlayState &state, const MapCanvas &canvas,
    const LayerEditCallbacks &cb = {});

// xf is optional: when non-null, the canvas-world pixel is first mapped back
// to the reference level via tgt_to_ref before the rmf-meter computation.
void draw_mouse_coord_hud(const MapCanvas &canvas, double ref_mpp,
                          const FloorTransform *xf = nullptr);

} // namespace imrmf::map_editor::canvas
