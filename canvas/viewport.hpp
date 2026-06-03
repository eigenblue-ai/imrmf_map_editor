// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#pragma once

#include "imgui/imgui.h"

#include <utility>

namespace imrmf::map_editor::canvas {

struct ViewState {
  float scale = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  bool view_initialized = false;
};

/// World→screen transform for a viewport centered at `canvas_center` with the
/// given ViewState (scale + pan offset).
ImVec2 view_world_to_screen(const ViewState &vs, ImVec2 canvas_center, double wx,
                            double wy);

/// Inverse of view_world_to_screen.
std::pair<double, double> view_screen_to_world(const ViewState &vs,
                                               ImVec2 canvas_center, ImVec2 sp);

/// Mouse-wheel zoom (cursor-centered) + middle-click drag pan. Mutates `vs`.
/// `hovered` is the result of ImGui::IsItemHovered() on the canvas widget.
void handle_pan_zoom(ViewState &vs, ImVec2 canvas_center, bool hovered);

} // namespace imrmf::map_editor::canvas
