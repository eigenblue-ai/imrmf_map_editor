// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/viewport.hpp"

#include <algorithm>

namespace imrmf::map_editor::canvas {

ImVec2 view_world_to_screen(const ViewState &vs, ImVec2 canvas_center, double wx,
                            double wy) {
  return ImVec2(
      canvas_center.x + (float)wx * vs.scale + vs.offset_x,
      canvas_center.y + (float)wy * vs.scale + vs.offset_y);
}

std::pair<double, double> view_screen_to_world(const ViewState &vs,
                                               ImVec2 canvas_center,
                                               ImVec2 sp) {
  return {
      ((sp.x - canvas_center.x - vs.offset_x) / vs.scale),
      ((sp.y - canvas_center.y - vs.offset_y) / vs.scale),
  };
}

void handle_pan_zoom(ViewState &vs, ImVec2 canvas_center, bool hovered) {
  ImGuiIO &io = ImGui::GetIO();
  if (hovered && io.MouseWheel != 0.0f) {
    auto [wxb, wyb] = view_screen_to_world(vs, canvas_center, io.MousePos);
    float factor = 1.0f + io.MouseWheel * 0.1f;
    vs.scale = std::max(0.05f, std::min(vs.scale * factor, 50.0f));
    auto [wxa, wya] = view_screen_to_world(vs, canvas_center, io.MousePos);
    vs.offset_x += (float)(wxa - wxb) * vs.scale;
    vs.offset_y += (float)(wya - wyb) * vs.scale;
  }
  static bool middle_panning = false;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    middle_panning = true;
  if (middle_panning) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      vs.offset_x += io.MouseDelta.x;
      vs.offset_y += io.MouseDelta.y;
    } else {
      middle_panning = false;
    }
  }
}

} // namespace imrmf::map_editor::canvas
