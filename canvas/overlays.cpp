// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/overlays.hpp"

#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>

namespace imrmf::map_editor::canvas {

namespace {

constexpr float kOverlayBgAlpha = 0.65f;
constexpr float kOverlayPadding = 8.0f;
constexpr ImGuiWindowFlags kOverlayWinFlags =
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

} // namespace

bool draw_level_selector_overlay(const Building &building, int &level_idx,
                                 const MapCanvas &canvas) {
  if (building.levels.empty())
    return false;
  level_idx = std::clamp(level_idx, 0, (int)building.levels.size() - 1);

  ImVec2 cp = canvas.canvas_pos();
  ImVec2 saved = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(
      ImVec2(cp.x + kOverlayPadding, cp.y + kOverlayPadding));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        ImVec4(0.05f, 0.05f, 0.05f, kOverlayBgAlpha));
  ImGui::BeginChild("##canvas_level_overlay", ImVec2(0, 0),
                    ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY,
                    kOverlayWinFlags);

  ImGui::TextUnformatted(building.name.empty() ? "Building"
                                               : building.name.c_str());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);

  bool changed = false;
  const std::string &current = building.levels[level_idx].name;
  if (ImGui::BeginCombo("##canvas_level_combo", current.c_str())) {
    for (int i = 0; i < (int)building.levels.size(); ++i) {
      bool sel = (i == level_idx);
      if (ImGui::Selectable(building.levels[i].name.c_str(), sel)) {
        if (i != level_idx) {
          level_idx = i;
          changed = true;
        }
      }
      if (sel)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SetCursorScreenPos(saved);
  return changed;
}

void draw_layers_overlay(
    Building &building, int level_idx,
    std::unordered_map<std::string, FloorplanSession> &fp_sessions,
    std::unordered_map<std::string, LayerSession> &layer_sessions,
    LayersOverlayState &state, const MapCanvas &canvas,
    const LayerEditCallbacks &cb) {
  if (building.levels.empty())
    return;
  level_idx = std::clamp(level_idx, 0, (int)building.levels.size() - 1);
  Level &level = building.levels[level_idx];
  const bool editable = (bool)cb.on_layer_commit;

  ImVec2 cp = canvas.canvas_pos();
  ImVec2 cs = canvas.canvas_size();
  float panel_w = state.expanded ? 280.0f : 90.0f;
  ImVec2 saved = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(
      ImVec2(cp.x + cs.x - panel_w - kOverlayPadding, cp.y + kOverlayPadding));
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        ImVec4(0.05f, 0.05f, 0.05f, kOverlayBgAlpha));
  ImGui::BeginChild("##canvas_layers_overlay", ImVec2(panel_w, 0),
                    ImGuiChildFlags_AutoResizeY, kOverlayWinFlags);

  if (ImGui::Button(state.expanded ? "Layers v" : "Layers >")) {
    state.expanded = !state.expanded;
  }

  if (state.expanded) {
    ImGui::Separator();

    FloorplanSession &fps = fp_sessions[level.name];
    ImGui::PushID("__fp");
    ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Floorplan");
    ImGui::Indent(8.0f);
    ImGui::Checkbox("visible##fp", &fps.visible);
    ImGui::SameLine();
    ImGui::Checkbox("invert##fp", &fps.invert);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##fp_alpha", &fps.alpha, 0.0f, 1.0f, "alpha %.2f");
    ImGui::Unindent(8.0f);
    ImGui::PopID();
    ImGui::Separator();

    for (int i = 0; i < (int)level.layers.size(); ++i) {
      Layer &L = level.layers[i];
      LayerSession &sess = layer_sessions[level.name + ":" + L.name];

      ImGui::PushID(i);

      char row[160];
      std::snprintf(row, sizeof(row), "%s##row", L.name.c_str());
      bool selected_row = (state.selected_layer == i);
      if (ImGui::Selectable(
              row, &selected_row, ImGuiSelectableFlags_AllowItemOverlap,
              ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() + 2.0f))) {
        state.selected_layer = selected_row ? i : -1;
      }

      if (editable && cb.on_layer_reorder && ImGui::IsItemActive() &&
          !ImGui::IsItemHovered()) {
        int next = i + (ImGui::GetMouseDragDelta(0).y < 0.0f ? -1 : 1);
        if (next >= 0 && next < (int)level.layers.size()) {
          std::swap(level.layers[i], level.layers[next]);
          if (state.selected_layer == i)
            state.selected_layer = next;
          else if (state.selected_layer == next)
            state.selected_layer = i;
          state.pending_reorder_commit = true;
          ImGui::ResetMouseDragDelta();
        }
      }

      ImGui::Indent(8.0f);

      bool vis = sess.visible.has_value() ? *sess.visible : L.visible;
      if (ImGui::Checkbox("visible", &vis)) {
        if (editable) {
          L.visible = vis;
          if (cb.on_layer_commit)
            cb.on_layer_commit(L);
        } else {
          sess.visible = vis;
        }
      }
      ImGui::SameLine();
      ImGui::Checkbox("invert", &sess.invert);

      float col[3] = {
          sess.color_r ? *sess.color_r : (float)L.color_r,
          sess.color_g ? *sess.color_g : (float)L.color_g,
          sess.color_b ? *sess.color_b : (float)L.color_b,
      };
      ImGui::SetNextItemWidth(-1);
      bool color_changed = ImGui::ColorEdit3("##color", col,
                                             ImGuiColorEditFlags_NoInputs |
                                                 ImGuiColorEditFlags_NoLabel);
      if (color_changed) {
        if (editable) {
          L.color_r = col[0];
          L.color_g = col[1];
          L.color_b = col[2];
        } else {
          sess.color_r = col[0];
          sess.color_g = col[1];
          sess.color_b = col[2];
        }
      }
      if (editable && cb.on_layer_commit &&
          ImGui::IsItemDeactivatedAfterEdit()) {
        cb.on_layer_commit(L);
      }

      float alpha = sess.alpha ? *sess.alpha : (float)L.color_a;
      ImGui::SetNextItemWidth(-1);
      bool alpha_changed =
          ImGui::SliderFloat("##alpha", &alpha, 0.0f, 1.0f, "alpha %.2f");
      if (alpha_changed) {
        if (editable) {
          L.color_a = alpha;
        } else {
          sess.alpha = alpha;
        }
      }
      if (editable && cb.on_layer_commit &&
          ImGui::IsItemDeactivatedAfterEdit()) {
        cb.on_layer_commit(L);
      }

      if (editable && cb.on_layer_delete) {
        if (ImGui::SmallButton("Delete")) {
          std::string name = L.name;
          ImGui::Unindent(8.0f);
          ImGui::PopID();
          ImGui::Separator();
          cb.on_layer_delete(name);
          ImGui::EndChild();
          ImGui::PopStyleColor();
          ImGui::SetCursorScreenPos(saved);
          return;
        }
      }

      ImGui::Unindent(8.0f);
      ImGui::PopID();
      ImGui::Separator();
    }

    if (editable && cb.on_layer_reorder && state.pending_reorder_commit &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      std::vector<std::string> order;
      order.reserve(level.layers.size());
      for (const Layer &q : level.layers)
        order.push_back(q.name);
      cb.on_layer_reorder(order);
      state.pending_reorder_commit = false;
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SetCursorScreenPos(saved);
}

} // namespace imrmf::map_editor::canvas
