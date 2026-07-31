// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#include "canvas/overlays.hpp"

#include "egb_imgui/icons.hpp"
#include "egb_imgui/theme.hpp"
#include "imgui/imgui.h"

#include <algorithm>
#include <cstdio>

namespace imrmf::map_editor::canvas {

namespace {

constexpr float kOverlayPadding = theme::metrics::overlay_edge;
constexpr float kOverlayRounding = theme::metrics::rounding;
inline ImVec2 overlay_inner_pad() {
  return ImVec2(theme::metrics::overlay_pad_x, theme::metrics::overlay_pad_y);
}
constexpr ImGuiWindowFlags kOverlayWinFlags =
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

// Themed overlay card, pair with end_overlay_card().
void begin_overlay_card(const char *id, ImVec2 size,
                        ImGuiChildFlags extra_flags) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        theme::with_alpha(theme::palette::surface, 0.92f));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::palette::border);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kOverlayRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, overlay_inner_pad());
  ImGui::BeginChild(id, size, extra_flags | ImGuiChildFlags_Border,
                    kOverlayWinFlags);
}
void end_overlay_card() {
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

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
  begin_overlay_card("##canvas_level_overlay", ImVec2(0, 0),
                     ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY);

  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Floor");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0f);

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

  end_overlay_card();
  ImGui::SetCursorScreenPos(saved);
  return changed;
}

void draw_layers_overlay(
    Building &building, int level_idx,
    std::unordered_map<std::string, FloorplanSession> &fp_sessions,
    std::unordered_map<std::string, LayerSession> &layer_sessions,
    LayersOverlayState &state, const MapCanvas &canvas,
    const LayerEditCallbacks &cb, const OverlayViewSettings &view) {
  if (building.levels.empty())
    return;
  level_idx = std::clamp(level_idx, 0, (int)building.levels.size() - 1);
  Level &level = building.levels[level_idx];
  const bool editable = (bool)cb.on_layer_commit;

  ImVec2 cp = canvas.canvas_pos();
  ImVec2 cs = canvas.canvas_size();
  const float btn_sz = ImGui::GetFrameHeight() + 8.0f;
  const float gap = 4.0f;
  ImVec2 saved = ImGui::GetCursorScreenPos();
  const bool has_view = (view.show_floors != nullptr);

  ImVec2 tb(cp.x + cs.x - btn_sz - kOverlayPadding, cp.y + kOverlayPadding);
  ImGui::SetCursorScreenPos(tb);
  if (ImGui::Button(ICON_MDI_LAYERS, ImVec2(btn_sz, btn_sz))) {
    state.expanded = !state.expanded;
    state.view_expanded = false;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Layers");
  if (has_view) {
    ImGui::SetCursorScreenPos(ImVec2(tb.x, tb.y + btn_sz + gap));
    if (ImGui::Button(ICON_MDI_EYE, ImVec2(btn_sz, btn_sz))) {
      state.view_expanded = !state.view_expanded;
      state.expanded = false;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("View settings");
  }

  if (state.expanded) {
    const float pane_w = 280.0f;
    ImGui::SetCursorScreenPos(
        ImVec2(tb.x - pane_w - kOverlayPadding, cp.y + kOverlayPadding));
    begin_overlay_card("##canvas_layers_overlay", ImVec2(pane_w, 0),
                       ImGuiChildFlags_AutoResizeY);

    {
      FloorplanSession &fps = fp_sessions[level.name];
      ImGui::PushID("__fp");
      ImGui::TextUnformatted("Floorplan");
      ImGui::Checkbox("visible##fp", &fps.visible);
      ImGui::SameLine();
      ImGui::Checkbox("invert##fp", &fps.invert);
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##fp_alpha", &fps.alpha, 0.0f, 1.0f, "alpha %.2f");
      ImGui::PopID();
      ImGui::Separator();

      for (int i = 0; i < (int)level.layers.size(); ++i) {
        Layer &L = level.layers[i];
        LayerSession &sess = layer_sessions[level.name + ":" + L.name];

        ImGui::PushID(i);
        ImGui::TextUnformatted(L.name.c_str());

        float col[4] = {
            sess.color_r ? *sess.color_r : (float)L.color_r,
            sess.color_g ? *sess.color_g : (float)L.color_g,
            sess.color_b ? *sess.color_b : (float)L.color_b,
            sess.alpha ? *sess.alpha : (float)L.color_a,
        };
        if (ImGui::ColorEdit4("##color", col,
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_NoLabel |
                                  ImGuiColorEditFlags_AlphaBar |
                                  ImGuiColorEditFlags_AlphaPreviewHalf)) {
          if (editable) {
            L.color_r = col[0];
            L.color_g = col[1];
            L.color_b = col[2];
            L.color_a = col[3];
          } else {
            sess.color_r = col[0];
            sess.color_g = col[1];
            sess.color_b = col[2];
            sess.alpha = col[3];
          }
        }
        if (editable && cb.on_layer_commit &&
            ImGui::IsItemDeactivatedAfterEdit())
          cb.on_layer_commit(L);
        ImGui::SameLine();
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

    end_overlay_card();
  }

  if (has_view && state.view_expanded) {
    const float pane_w = 200.0f;
    ImGui::SetCursorScreenPos(
        ImVec2(tb.x - pane_w - kOverlayPadding, cp.y + kOverlayPadding));
    begin_overlay_card("##canvas_view_overlay", ImVec2(pane_w, 0),
                       ImGuiChildFlags_AutoResizeY);
    if (view.show_fiducials)
      ImGui::Checkbox("Fiducials", view.show_fiducials);
    if (view.show_floors)
      ImGui::Checkbox("Floors", view.show_floors);
    if (view.show_walls)
      ImGui::Checkbox("Walls", view.show_walls);
    if (view.show_doors)
      ImGui::Checkbox("Doors", view.show_doors);
    if (view.show_measurements)
      ImGui::Checkbox("Measurements", view.show_measurements);
    end_overlay_card();
  }

  ImGui::SetCursorScreenPos(saved);
}

void draw_mouse_coord_hud(const MapCanvas &c, double ref_mpp,
                          const FloorTransform *xf) {
  ImVec2 m = ImGui::GetIO().MousePos;
  ImVec2 cp = c.canvas_pos();
  ImVec2 cs = c.canvas_size();
  if (m.x < cp.x || m.x > cp.x + cs.x || m.y < cp.y || m.y > cp.y + cs.y)
    return;
  auto [px, py] = c.screen_to_world(m);
  double rx = px, ry = py;
  if (xf) {
    auto p = tgt_to_ref(*xf, px, py);
    rx = p.first;
    ry = p.second;
  }
  double eff = ref_mpp > 0.0 ? ref_mpp : 1.0;
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "px (%.0f, %.0f)  |  rmf m (%.3f, %.3f)  |  zoom %.0f%%", px,
                py, rx * eff, -ry * eff, (double)c.view_state().scale * 100.0);
  ImVec2 sz = ImGui::CalcTextSize(buf);
  ImVec2 pos(cp.x + kOverlayPadding + 8.0f,
             cp.y + cs.y - sz.y - kOverlayPadding - 5.0f);
  ImDrawList *dl = c.draw_list();
  ImVec2 r0(pos.x - 8, pos.y - 5), r1(pos.x + sz.x + 8, pos.y + sz.y + 5);
  dl->AddRectFilled(
      r0, r1,
      ImGui::GetColorU32(theme::with_alpha(theme::palette::surface, 0.92f)),
      kOverlayRounding);
  dl->AddRect(r0, r1, ImGui::GetColorU32(theme::palette::border),
              kOverlayRounding);
  dl->AddText(pos, ImGui::GetColorU32(theme::palette::text), buf);
}

void draw_mouse_coord_hud(const MapCanvas &c, const Building &building,
                          int level_idx) {
  ImVec2 m = ImGui::GetIO().MousePos;
  ImVec2 cp = c.canvas_pos();
  ImVec2 cs = c.canvas_size();
  if (m.x < cp.x || m.x > cp.x + cs.x || m.y < cp.y || m.y > cp.y + cs.y)
    return;
  auto [px, py] = c.screen_to_world(m);
  auto [rmf_x, rmf_y] = level_px_to_rmf(building, level_idx, px, py);
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "px (%.0f, %.0f)  |  rmf m (%.3f, %.3f)  |  zoom %.0f%%", px,
                py, rmf_x, rmf_y, (double)c.view_state().scale * 100.0);
  ImVec2 sz = ImGui::CalcTextSize(buf);
  ImVec2 pos(cp.x + kOverlayPadding + 8.0f,
             cp.y + cs.y - sz.y - kOverlayPadding - 5.0f);
  ImDrawList *dl = c.draw_list();
  ImVec2 r0(pos.x - 8, pos.y - 5), r1(pos.x + sz.x + 8, pos.y + sz.y + 5);
  dl->AddRectFilled(
      r0, r1,
      ImGui::GetColorU32(theme::with_alpha(theme::palette::surface, 0.92f)),
      kOverlayRounding);
  dl->AddRect(r0, r1, ImGui::GetColorU32(theme::palette::border),
              kOverlayRounding);
  dl->AddText(pos, ImGui::GetColorU32(theme::palette::text), buf);
}

} // namespace imrmf::map_editor::canvas
