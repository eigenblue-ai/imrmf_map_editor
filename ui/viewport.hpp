#pragma once

#include "imgui/imgui.h"

#include <algorithm>
#include <utility>

// Pan/zoom transform for a 2D world-space canvas. ImGui only.
namespace ui {

// One per canvas. `panning` tracks an in-progress middle-drag.
struct ViewState {
  float scale = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  bool view_initialized = false;
  bool panning = false;
};

// World -> screen for a viewport centered at `canvas_center`.
inline ImVec2 view_world_to_screen(const ViewState &vs, ImVec2 canvas_center,
                                   double wx, double wy) {
  return ImVec2(canvas_center.x + (float)wx * vs.scale + vs.offset_x,
                canvas_center.y + (float)wy * vs.scale + vs.offset_y);
}

// Inverse of view_world_to_screen.
inline std::pair<double, double>
view_screen_to_world(const ViewState &vs, ImVec2 canvas_center, ImVec2 sp) {
  return {
      (sp.x - canvas_center.x - vs.offset_x) / vs.scale,
      (sp.y - canvas_center.y - vs.offset_y) / vs.scale,
  };
}

// Cursor-centered mouse-wheel zoom + middle-drag pan. Mutates `vs`.
// `hovered` is ImGui::IsItemHovered() on the canvas widget.
inline void handle_pan_zoom(ViewState &vs, ImVec2 canvas_center, bool hovered) {
  ImGuiIO &io = ImGui::GetIO();
  if (hovered && io.MouseWheel != 0.0f) {
    auto [wxb, wyb] = view_screen_to_world(vs, canvas_center, io.MousePos);
    const float factor = 1.0f + io.MouseWheel * 0.1f;
    vs.scale = std::max(0.05f, std::min(vs.scale * factor, 50.0f));
    auto [wxa, wya] = view_screen_to_world(vs, canvas_center, io.MousePos);
    vs.offset_x += (float)(wxa - wxb) * vs.scale;
    vs.offset_y += (float)(wya - wyb) * vs.scale;
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    vs.panning = true;
  if (vs.panning) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      vs.offset_x += io.MouseDelta.x;
      vs.offset_y += io.MouseDelta.y;
    } else {
      vs.panning = false;
    }
  }
}

} // namespace ui
