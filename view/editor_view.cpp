// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#define IMGUI_DEFINE_MATH_OPERATORS
#include "view/editor_view.hpp"

#include "imgui/imgui.h"
#include "imgui/misc/cpp/imgui_stdlib.h"
#include "model/yaml_io.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace imrmf::map_editor {

namespace {

constexpr float kHitRadiusPx = 8.0f;
constexpr float kLaneHitPx = 6.0f;

struct VertexParamSpec {
  const char *key;
  ParamType type;
};
const std::array<VertexParamSpec, 13> kVertexParams = {{
    {"is_charger", ParamType::BOOL},
    {"is_parking_spot", ParamType::BOOL},
    {"is_holding_point", ParamType::BOOL},
    {"is_passthrough_point", ParamType::BOOL},
    {"is_cleaning_zone", ParamType::BOOL},
    {"dock_name", ParamType::STRING},
    {"pickup_dispenser", ParamType::STRING},
    {"dropoff_ingestor", ParamType::STRING},
    {"spawn_robot_type", ParamType::STRING},
    {"spawn_robot_name", ParamType::STRING},
    {"human_goal_set_name", ParamType::STRING},
    {"mutex", ParamType::STRING},
    {"merge_radius", ParamType::DOUBLE},
}};

struct LaneParamSpec {
  const char *key;
  ParamType type;
};
const std::array<LaneParamSpec, 7> kLaneParams = {{
    {"bidirectional", ParamType::BOOL},
    {"orientation", ParamType::STRING},
    {"graph_idx", ParamType::INT},
    {"speed_limit", ParamType::DOUBLE},
    {"mutex", ParamType::STRING},
    {"demo_mock_floor_name", ParamType::STRING},
    {"demo_mock_lift_name", ParamType::STRING},
}};

float dist_point_segment(float px, float py, float ax, float ay, float bx,
                         float by) {
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  if (len2 < 1e-6f) {
    float ex = px - ax, ey = py - ay;
    return std::sqrt(ex * ex + ey * ey);
  }
  float t = ((px - ax) * dx + (py - ay) * dy) / len2;
  t = std::max(0.0f, std::min(1.0f, t));
  float qx = ax + t * dx, qy = ay + t * dy;
  float ex = px - qx, ey = py - qy;
  return std::sqrt(ex * ex + ey * ey);
}

// Multi-select helpers.
bool is_selected(const std::vector<int> &v, int idx) {
  return std::find(v.begin(), v.end(), idx) != v.end();
}
void add_to_selection(std::vector<int> &v, int idx) {
  if (!is_selected(v, idx))
    v.push_back(idx);
}
void remove_from_selection(std::vector<int> &v, int idx) {
  v.erase(std::remove(v.begin(), v.end(), idx), v.end());
}

std::pair<double, double> snap_axis_or_diagonal(double dx, double dy) {
  double adx = std::abs(dx), ady = std::abs(dy);
  if (adx >= ady * 2.414)
    return {dx, 0.0};
  if (ady >= adx * 2.414)
    return {0.0, dy};
  double m = (adx + ady) * 0.5;
  return {(dx >= 0 ? 1.0 : -1.0) * m, (dy >= 0 ? 1.0 : -1.0) * m};
}

// Align operations on a set of selected vertices.
enum class AlignDir { Horizontal, Vertical }; // H = align Y, V = align X
enum class AlignTo {
  Average,
  Min,
  Max
}; // Min = topmost/leftmost, Max = bottommost/rightmost
void apply_align(Level &level, const std::vector<int> &sel, AlignDir dir,
                 AlignTo to) {
  if (sel.size() < 2)
    return;
  double target = 0.0;
  if (to == AlignTo::Average) {
    double sum = 0.0;
    for (int i : sel)
      sum += (dir == AlignDir::Horizontal) ? level.vertices[i].y
                                           : level.vertices[i].x;
    target = sum / (double)sel.size();
  } else {
    double best = (dir == AlignDir::Horizontal) ? level.vertices[sel[0]].y
                                                : level.vertices[sel[0]].x;
    for (int i : sel) {
      double v = (dir == AlignDir::Horizontal) ? level.vertices[i].y
                                               : level.vertices[i].x;
      if (to == AlignTo::Min)
        best = std::min(best, v);
      else
        best = std::max(best, v);
    }
    target = best;
  }
  for (int i : sel) {
    if (dir == AlignDir::Horizontal)
      level.vertices[i].y = target;
    else
      level.vertices[i].x = target;
  }
}

// Fine-grained Yjs op wrappers
#ifdef __EMSCRIPTEN__
// Forward decls of the EM_JS bridges defined below.
extern "C" {
void mevjs_vertex_add(const char *level, const char *yaml);
void mevjs_vertex_replace(const char *level, int idx, const char *yaml);
void mevjs_vertex_delete(const char *level, int idx);
void mevjs_lane_add(const char *level, const char *yaml);
void mevjs_lane_replace(const char *level, int idx, const char *yaml);
void mevjs_lane_delete(const char *level, int idx);
void mevjs_layer_set(const char *level, const char *layer_name,
                     const char *yaml);
void mevjs_layer_delete(const char *level, const char *layer_name);
void mevjs_layer_reorder(const char *level, const char *names_json);
}
#endif

void yjs_op_vertex_add(const std::string &level, const Vertex &v) {
#ifdef __EMSCRIPTEN__
  std::string y = serialize_vertex(v);
  mevjs_vertex_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)v;
#endif
}
void yjs_op_vertex_replace(const std::string &level, int idx, const Vertex &v) {
#ifdef __EMSCRIPTEN__
  std::string y = serialize_vertex(v);
  mevjs_vertex_replace(level.c_str(), idx, y.c_str());
#else
  (void)level;
  (void)idx;
  (void)v;
#endif
}
void yjs_op_vertex_delete(const std::string &level, int idx) {
#ifdef __EMSCRIPTEN__
  mevjs_vertex_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_lane_add(const std::string &level, const Lane &l) {
#ifdef __EMSCRIPTEN__
  std::string y = serialize_lane(l);
  mevjs_lane_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)l;
#endif
}
void yjs_op_lane_replace(const std::string &level, int idx, const Lane &l) {
#ifdef __EMSCRIPTEN__
  std::string y = serialize_lane(l);
  mevjs_lane_replace(level.c_str(), idx, y.c_str());
#else
  (void)level;
  (void)idx;
  (void)l;
#endif
}
void yjs_op_lane_delete(const std::string &level, int idx) {
#ifdef __EMSCRIPTEN__
  mevjs_lane_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_layer_set(const std::string &level, const Layer &L) {
#ifdef __EMSCRIPTEN__
  std::string y = serialize_layer(L);
  mevjs_layer_set(level.c_str(), L.name.c_str(), y.c_str());
#else
  (void)level;
  (void)L;
#endif
}
void yjs_op_layer_delete(const std::string &level, const std::string &name) {
#ifdef __EMSCRIPTEN__
  mevjs_layer_delete(level.c_str(), name.c_str());
#else
  (void)level;
  (void)name;
#endif
}
void yjs_op_layer_reorder(const std::string &level,
                          const std::vector<std::string> &names) {
#ifdef __EMSCRIPTEN__
  // Emit a JSON array of layer names — JSON.parse on the JS side.
  std::string buf = "[";
  for (size_t i = 0; i < names.size(); ++i) {
    if (i)
      buf += ',';
    buf += '"';
    for (char c : names[i]) {
      if (c == '\\' || c == '"')
        buf += '\\';
      buf += c;
    }
    buf += '"';
  }
  buf += "]";
  mevjs_layer_reorder(level.c_str(), buf.c_str());
#else
  (void)level;
  (void)names;
#endif
}

void flush_pending_vertex(Level &level, EditorState &state) {
  if (state.pending_commit_vertex < 0)
    return;
  if (state.pending_commit_vertex < (int)level.vertices.size()) {
    yjs_op_vertex_replace(level.name, state.pending_commit_vertex,
                          level.vertices[state.pending_commit_vertex]);
  }
  state.pending_commit_vertex = -1;
  state.pending_commit_time = 0.0;
}
void flush_pending_lane(Level &level, EditorState &state) {
  if (state.pending_commit_lane < 0)
    return;
  if (state.pending_commit_lane < (int)level.lanes.size()) {
    yjs_op_lane_replace(level.name, state.pending_commit_lane,
                        level.lanes[state.pending_commit_lane]);
  }
  state.pending_commit_lane = -1;
  state.pending_commit_time = 0.0;
}
void flush_pending_layer(Level &level, EditorState &state) {
  if (state.pending_commit_layer.empty())
    return;
  for (const auto &L : level.layers) {
    if (L.name == state.pending_commit_layer) {
      yjs_op_layer_set(level.name, L);
      break;
    }
  }
  state.pending_commit_layer.clear();
  state.pending_commit_time = 0.0;
}
void flush_all_pending(Level &level, EditorState &state) {
  flush_pending_vertex(level, state);
  flush_pending_lane(level, state);
  flush_pending_layer(level, state);
}

constexpr double kIdleCommitSeconds = 2.0;

void init_default_lane_params(Lane &l) {
  auto need = [&](const char *k, ParamValue v) {
    if (l.params.find(k) == l.params.end())
      l.params[k] = v;
  };
  need("bidirectional", ParamValue::make_bool(true));
  need("orientation", ParamValue::make_string(""));
  need("graph_idx", ParamValue::make_int(0));
  need("speed_limit", ParamValue::make_double(0.0));
  need("mutex", ParamValue::make_string(""));
  need("demo_mock_floor_name", ParamValue::make_string(""));
  need("demo_mock_lift_name", ParamValue::make_string(""));
}

// Strict interior segment-segment intersection. Tangents and endpoints don't
// count so a dissection drag can't split itself.
bool segment_intersect(double ax, double ay, double bx, double by, double cx,
                       double cy, double dx, double dy, double &ix,
                       double &iy) {
  double rx = bx - ax, ry = by - ay;
  double sx = dx - cx, sy = dy - cy;
  double denom = rx * sy - ry * sx;
  if (std::abs(denom) < 1e-9)
    return false;
  double t = ((cx - ax) * sy - (cy - ay) * sx) / denom;
  double u = ((cx - ax) * ry - (cy - ay) * rx) / denom;
  constexpr double eps = 1e-6;
  if (t <= eps || t >= 1.0 - eps || u <= eps || u >= 1.0 - eps)
    return false;
  ix = ax + t * rx;
  iy = ay + t * ry;
  return true;
}

void draw_orientation_combo(std::map<std::string, ParamValue> &params,
                            bool &dirty, bool &commit) {
  auto &pv = params["orientation"];
  if (pv.type != ParamType::STRING) {
    pv.type = ParamType::STRING;
    pv.s.clear();
  }
  const char *items[] = {"(none)", "forward", "backward"};
  int current = 0;
  if (pv.s == "forward")
    current = 1;
  else if (pv.s == "backward")
    current = 2;
  ImGui::PushID("orientation");
  if (ImGui::Combo("orientation", &current, items, 3)) {
    pv.s = (current == 1) ? "forward" : (current == 2) ? "backward" : "";
    dirty = true;
    commit = true;
  }
  ImGui::PopID();
}

void draw_param_editor(std::map<std::string, ParamValue> &params,
                       const char *key, ParamType type, bool &dirty,
                       bool &commit) {
  auto &pv = params[key];
  if (pv.type != type) {
    pv.type = type;
    pv.s.clear();
    pv.i = 0;
    pv.d = 0.0;
    pv.b = false;
  }
  ImGui::PushID(key);
  switch (type) {
  case ParamType::BOOL: {
    bool v = pv.b;
    if (ImGui::Checkbox(key, &v)) {
      pv.b = v;
      dirty = true;
      commit = true;
    }
    break;
  }
  case ParamType::STRING: {
    std::string v = pv.s;
    if (ImGui::InputText(key, &v)) {
      pv.s = std::move(v);
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  case ParamType::INT: {
    int v = pv.i;
    if (ImGui::InputInt(key, &v)) {
      pv.i = v;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  case ParamType::DOUBLE: {
    double v = pv.d;
    float fv = (float)v;
    if (ImGui::InputFloat(key, &fv, 0.0f, 0.0f, "%.4f")) {
      pv.d = (double)fv;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  }
  ImGui::PopID();
}

} // namespace

namespace {

#ifdef __EMSCRIPTEN__
// Yjs status string: "connecting", "connected", "disconnected", or "".
EM_JS(const char *, map_editor_yjs_status, (), {
  if (!window.imrmf.yjs)
    return stringToNewUTF8("");
  const s = window.imrmf.yjs.getStatus() || "";
  return stringToNewUTF8(s);
});

EM_JS(int, map_editor_yjs_synced, (), {
  if (!window.imrmf.yjs)
    return 0;
  return window.imrmf.yjs.isSynced() ? 1 : 0;
});

EM_JS(int, map_editor_yjs_can_undo, (), {
  return (window.imrmf && window.imrmf.yjs && window.imrmf.yjs.canUndo &&
          window.imrmf.yjs.canUndo()) ? 1 : 0;
});
EM_JS(int, map_editor_yjs_can_redo, (), {
  return (window.imrmf && window.imrmf.yjs && window.imrmf.yjs.canRedo &&
          window.imrmf.yjs.canRedo()) ? 1 : 0;
});
EM_JS(void, map_editor_yjs_undo, (), {
  if (window.imrmf.yjs && window.imrmf.yjs.undo)
    window.imrmf.yjs.undo();
});

EM_JS(void, map_editor_yjs_redo, (), {
  if (window.imrmf.yjs && window.imrmf.yjs.redo)
    window.imrmf.yjs.redo();
});

// Fine-grained CRDT ops: each EM_JS call is one Yjs transaction (origin
// 'local') that mutates the Doc and broadcasts without bouncing back to us.
EM_JS(void, mevjs_vertex_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.vertexAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_vertex_replace,
      (const char *level, int idx, const char *yaml), {
        if (window.imrmf.yjs)
          window.imrmf.yjs.vertexReplace(UTF8ToString(level), idx,
                                         UTF8ToString(yaml));
      });
EM_JS(void, mevjs_vertex_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.vertexDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_lane_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.laneAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_lane_replace, (const char *level, int idx, const char *yaml),
      {
        if (window.imrmf.yjs)
          window.imrmf.yjs.laneReplace(UTF8ToString(level), idx,
                                       UTF8ToString(yaml));
      });
EM_JS(void, mevjs_lane_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.laneDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_layer_set,
      (const char *level, const char *layer_name, const char *yaml), {
        if (window.imrmf.yjs)
          window.imrmf.yjs.layerSet(UTF8ToString(level),
                                    UTF8ToString(layer_name),
                                    UTF8ToString(yaml));
      });
EM_JS(void, mevjs_layer_delete, (const char *level, const char *layer_name), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.layerDelete(UTF8ToString(level), UTF8ToString(layer_name));
});
EM_JS(void, mevjs_layer_reorder, (const char *level, const char *names_json), {
  if (!window.imrmf.yjs)
    return;
  try {
    const names = JSON.parse(UTF8ToString(names_json));
    window.imrmf.yjs.layerReorder(UTF8ToString(level), names);
  } catch (e) {
    console.error('[yjs] layer reorder parse failed:', e);
  }
});

#endif // __EMSCRIPTEN__

} // namespace

EditorView::EditorView(std::string /*image_root_unused*/,
                       std::string building_id)
    : building_id_(std::move(building_id)),
      texture_provider_(std::make_unique<canvas::HttpTextureProvider>()),
      canvas_(building_id_, texture_provider_.get()) {}

EditorView::~EditorView() = default;

void EditorView::draw(Building &building, EditorState &state,
                      const std::function<void()> &save_callback,
                      const TopBarHooks &top_bar) {
  if (building.levels.empty()) {
    ImGui::Text("No levels loaded.");
    return;
  }
  state.level_idx =
      std::max(0, std::min(state.level_idx, (int)building.levels.size() - 1));

  // Flush pending edits against the old level before switching.
  if (state.last_drawn_level_idx >= 0 &&
      state.last_drawn_level_idx != state.level_idx &&
      state.last_drawn_level_idx < (int)building.levels.size()) {
    flush_all_pending(building.levels[state.last_drawn_level_idx], state);
  }
  state.last_drawn_level_idx = state.level_idx;

  draw_top_bar(building, state, save_callback, top_bar);
  ImGui::Separator();

  const float right_col_w = 320.0f;
  ImVec2 region = ImGui::GetContentRegionAvail();
  float status_h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
  ImVec2 canvas_region(region.x - right_col_w - 8.0f, region.y - status_h);
  if (canvas_region.x < 100.0f)
    canvas_region.x = 100.0f;

  ImGui::BeginChild("canvas_region", canvas_region, false,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  draw_canvas(building, state);
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("right_col", ImVec2(right_col_w, region.y - status_h),
                    false);
  draw_add_layer_section(building, state);
  ImGui::Separator();
  draw_attribute_panel(building, state);
  ImGui::EndChild();

  draw_status_bar(state);

  if (!ImGui::GetIO().WantTextInput) {
    // `S` collides with Ctrl+S (Save); require no modifier.
    if (ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().KeyCtrl &&
        !ImGui::GetIO().KeySuper)
      state.mode = Mode::Pan;
    if (ImGui::IsKeyPressed(ImGuiKey_V))
      state.mode = Mode::Vertex;
    if (ImGui::IsKeyPressed(ImGuiKey_L))
      state.mode = Mode::Lane;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
      state.pending_lane_start = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
      Level &level = building.levels[state.level_idx];
      if (!state.selected_lanes.empty()) {
        auto sel = state.selected_lanes;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int i : sel) {
          yjs_op_lane_delete(level.name, i);
          delete_lane(level, i);
        }
        state.selected_lanes.clear();
      }
      if (state.selected_vertices.size() > 1) {
        auto sel = state.selected_vertices;
        std::sort(sel.begin(), sel.end(), std::greater<int>());
        for (int i : sel) {
          yjs_op_vertex_delete(level.name, i);
          delete_vertex(level, i);
        }
        state.selected_vertices.clear();
      } else if (state.selected_vertices.size() == 1) {
        int v = state.selected_vertices[0];
        auto refs = lanes_referencing_vertex(level, v);
        if (refs.empty()) {
          yjs_op_vertex_delete(level.name, v);
          delete_vertex(level, v);
          state.selected_vertices.clear();
        } else {
          state.pending_vertex_delete = true;
          state.pending_vertex_delete_idx = v;
        }
      }
    }
  }
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) &&
      ImGui::IsKeyPressed(ImGuiKey_S)) {
    save_callback();
  }
#ifdef __EMSCRIPTEN__
  // Yjs-backed undo / redo. Captured-timeout (500ms in JS) batches contiguous
  // small pushes (e.g. a single drag) into one undo step.
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) &&
      !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Z)) {
    if (ImGui::GetIO().KeyShift) {
      map_editor_yjs_redo();
    } else {
      map_editor_yjs_undo();
    }
  }
  if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) &&
      !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Y)) {
    map_editor_yjs_redo();
  }
#endif

  // Idle-commit if the pending edit has been quiet long enough.
  if (state.pending_commit_time > 0.0 &&
      (ImGui::GetTime() - state.pending_commit_time) > kIdleCommitSeconds) {
    flush_all_pending(building.levels[state.level_idx], state);
  }

  if (state.pending_vertex_delete) {
    ImGui::OpenPopup("Confirm delete");
    state.pending_vertex_delete = false;
  }
  if (ImGui::BeginPopupModal("Confirm delete", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    Level &level = building.levels[state.level_idx];
    int idx = state.pending_vertex_delete_idx;
    auto refs = (idx >= 0 && idx < (int)level.vertices.size())
                    ? lanes_referencing_vertex(level, idx)
                    : std::vector<int>{};
    ImGui::Text("Vertex %d is referenced by %d lane(s).", idx,
                (int)refs.size());
    ImGui::Text("Delete vertex and dependent lanes?");
    ImGui::Separator();
    if (ImGui::Button("Delete", ImVec2(120, 0))) {
      yjs_op_vertex_delete(level.name, idx);
      delete_vertex(level, idx);
      state.selected_vertices.clear();
      state.pending_vertex_delete_idx = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      state.pending_vertex_delete_idx = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void EditorView::draw_top_bar(Building &building, EditorState &state,
                              const std::function<void()> &save_callback,
                              const TopBarHooks &top_bar) {
  (void)save_callback;
  Level &level = building.levels[state.level_idx];

  if (!top_bar.connection_label.empty()) {
    constexpr size_t kMaxLabel = 64;
    std::string shown = top_bar.connection_label;
    if (shown.size() > kMaxLabel) {
      shown = shown.substr(0, kMaxLabel - 1) + "\xE2\x80\xA6"; // utf-8 ellipsis
    }
    ImGui::TextDisabled("%s", shown.c_str());
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", top_bar.connection_label.c_str());
    ImGui::SameLine();
  }
  if (top_bar.can_disconnect && top_bar.on_disconnect) {
    if (ImGui::SmallButton("Disconnect"))
      top_bar.on_disconnect();
    ImGui::SameLine();
  }
  ImGui::TextDisabled("|");
  ImGui::SameLine();

  bool can_undo = false, can_redo = false;
#ifdef __EMSCRIPTEN__
  can_undo = map_editor_yjs_can_undo() != 0;
  can_redo = map_editor_yjs_can_redo() != 0;
#endif
  if (!can_undo)
    ImGui::BeginDisabled();
  if (ImGui::Button("Undo")) {
#ifdef __EMSCRIPTEN__
    map_editor_yjs_undo();
#endif
  }
  if (!can_undo)
    ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Ctrl+Z");
  ImGui::SameLine();
  if (!can_redo)
    ImGui::BeginDisabled();
  if (ImGui::Button("Redo")) {
#ifdef __EMSCRIPTEN__
    map_editor_yjs_redo();
#endif
  }
  if (!can_redo)
    ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Ctrl+Y / Ctrl+Shift+Z");
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();

  auto mode_button = [&](const char *label, Mode m) {
    bool active = (state.mode == m);
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 130, 200, 255));
    if (ImGui::Button(label)) {
      state.mode = m;
      state.pending_lane_start = -1;
    }
    if (active)
      ImGui::PopStyleColor();
    ImGui::SameLine();
  };
  mode_button("Select [S]", Mode::Pan);
  mode_button("Vertex [V]", Mode::Vertex);
  mode_button("Lane [L]", Mode::Lane);

  if (state.dirty) {
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(modified)");
  } else {
    ImGui::TextDisabled("(saved)");
  }

  const int nv = (int)state.selected_vertices.size();
  const int nl = (int)state.selected_lanes.size();
  if (nv == 0 && nl == 0)
    return;

  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  if (nv > 0 && nl > 0)
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%d vertex / %d lane%s",
                       nv, nl, nl == 1 ? "" : "s");
  else if (nv > 0)
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%d vertex%s", nv,
                       nv == 1 ? "" : "es");
  else
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%d lane%s", nl,
                       nl == 1 ? "" : "s");
  ImGui::SameLine();

  std::vector<int> implied_lanes;
  for (int v : state.selected_vertices) {
    for (int li : lanes_referencing_vertex(level, v)) {
      if (std::find(state.selected_lanes.begin(), state.selected_lanes.end(),
                    li) == state.selected_lanes.end() &&
          std::find(implied_lanes.begin(), implied_lanes.end(), li) ==
              implied_lanes.end())
        implied_lanes.push_back(li);
    }
  }
  const bool any_lanes_affected = nl > 0 || !implied_lanes.empty();

  auto delete_lane_indices = [&](std::vector<int> lanes) {
    std::sort(lanes.begin(), lanes.end(), std::greater<int>());
    lanes.erase(std::unique(lanes.begin(), lanes.end()), lanes.end());
    for (int i : lanes) {
      yjs_op_lane_delete(level.name, i);
      delete_lane(level, i);
    }
  };
  auto delete_all_selected = [&]() {
    // Lanes first (descending) so indices stay valid; vertex deletion then
    // cascades through Yjs to any remaining referencing lanes.
    delete_lane_indices(state.selected_lanes);
    auto sel = state.selected_vertices;
    std::sort(sel.begin(), sel.end(), std::greater<int>());
    for (int i : sel) {
      yjs_op_vertex_delete(level.name, i);
      delete_vertex(level, i);
    }
    state.selected_vertices.clear();
    state.selected_lanes.clear();
  };
  auto delete_only_lanes = [&]() {
    std::vector<int> all_lanes = state.selected_lanes;
    for (int li : implied_lanes) {
      if (std::find(all_lanes.begin(), all_lanes.end(), li) == all_lanes.end())
        all_lanes.push_back(li);
    }
    delete_lane_indices(all_lanes);
    state.selected_lanes.clear();
  };

  if (nv > 0 && any_lanes_affected) {
    if (ImGui::Button("Delete \xE2\x96\xBE"))
      ImGui::OpenPopup("delete_combo_popup");
    if (ImGui::BeginPopup("delete_combo_popup")) {
      if (ImGui::Selectable("Delete vertices and lanes"))
        delete_all_selected();
      if (ImGui::Selectable("Delete lanes only"))
        delete_only_lanes();
      ImGui::EndPopup();
    }
  } else if (nv > 0) {
    if (ImGui::Button("Delete"))
      delete_all_selected();
  } else {
    if (ImGui::Button("Delete"))
      delete_only_lanes();
  }

  if (nv >= 2) {
    auto align_and_push = [&](AlignDir d, AlignTo t) {
      apply_align(level, state.selected_vertices, d, t);
      for (int vi : state.selected_vertices) {
        if (vi >= 0 && vi < (int)level.vertices.size())
          yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
      }
    };
    ImGui::SameLine();
    if (ImGui::Button("Align H"))
      ImGui::OpenPopup("align_h_popup");
    if (ImGui::BeginPopup("align_h_popup")) {
      if (ImGui::Selectable("To average Y"))
        align_and_push(AlignDir::Horizontal, AlignTo::Average);
      if (ImGui::Selectable("To topmost Y"))
        align_and_push(AlignDir::Horizontal, AlignTo::Min);
      if (ImGui::Selectable("To bottommost Y"))
        align_and_push(AlignDir::Horizontal, AlignTo::Max);
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Align V"))
      ImGui::OpenPopup("align_v_popup");
    if (ImGui::BeginPopup("align_v_popup")) {
      if (ImGui::Selectable("To average X"))
        align_and_push(AlignDir::Vertical, AlignTo::Average);
      if (ImGui::Selectable("To leftmost X"))
        align_and_push(AlignDir::Vertical, AlignTo::Min);
      if (ImGui::Selectable("To rightmost X"))
        align_and_push(AlignDir::Vertical, AlignTo::Max);
      ImGui::EndPopup();
    }
  }
}

void EditorView::draw_canvas(Building &building, EditorState &state) {
  Level &level = building.levels[state.level_idx];

  canvas::DrawOptions opts;
  opts.floorplan_sessions = &state.floorplan_session;
  opts.layer_sessions = &state.layer_session;
  opts.show_vertex_names = true;
  opts.after_draw = [&](const canvas::MapCanvas &c) {
    for (int li : state.selected_lanes) {
      if (li < 0 || li >= (int)level.lanes.size())
        continue;
      const Lane &l = level.lanes[li];
      if (l.start_idx >= 0 && l.start_idx < (int)level.vertices.size() &&
          l.end_idx >= 0 && l.end_idx < (int)level.vertices.size()) {
        ImVec2 a = c.world_to_screen(level.vertices[l.start_idx].x,
                                     level.vertices[l.start_idx].y);
        ImVec2 b = c.world_to_screen(level.vertices[l.end_idx].x,
                                     level.vertices[l.end_idx].y);
        c.draw_list()->AddLine(a, b, canvas::lane_color(l), 7.0f);
      }
    }
    for (int i : state.selected_vertices) {
      if (i < 0 || i >= (int)level.vertices.size())
        continue;
      ImVec2 p = c.world_to_screen(level.vertices[i].x, level.vertices[i].y);
      c.draw_list()->AddCircle(p, 7.5f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
    }
    if (state.pending_lane_start >= 0 &&
        state.pending_lane_start < (int)level.vertices.size()) {
      const Vertex &v = level.vertices[state.pending_lane_start];
      ImVec2 p = c.world_to_screen(v.x, v.y);
      c.draw_list()->AddCircle(p, 9.5f, IM_COL32(80, 255, 120, 255), 0, 2.0f);
    }
    if (marquee_active_) {
      ImVec2 a = marquee_start_;
      ImVec2 b = ImGui::GetIO().MousePos;
      ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
      ImVec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
      c.draw_list()->AddRectFilled(lo, hi, IM_COL32(120, 170, 255, 40));
      c.draw_list()->AddRect(lo, hi, IM_COL32(180, 210, 255, 200), 0.0f, 0,
                             1.5f);
    }
  };
  canvas_.draw(building, state.level_idx, opts);

  int new_idx = state.level_idx;
  if (canvas::draw_level_selector_overlay(building, new_idx, canvas_) &&
      new_idx != state.level_idx) {
    flush_all_pending(building.levels[state.level_idx], state);
    state.level_idx = new_idx;
    state.selected_vertices.clear();
    state.selected_lanes.clear();
    state.pending_lane_start = -1;
    state.selected_layer = -1;
    reset_view();
  }

  canvas::LayerEditCallbacks lcb;
  lcb.on_layer_commit = [&](const Layer &L) {
    yjs_op_layer_set(building.levels[state.level_idx].name, L);
  };
  lcb.on_layer_delete = [&](const std::string &name) {
    Level &cur = building.levels[state.level_idx];
    for (size_t i = 0; i < cur.layers.size(); ++i) {
      if (cur.layers[i].name == name) {
        yjs_op_layer_delete(cur.name, name);
        cur.layers.erase(cur.layers.begin() + i);
        if (state.selected_layer == (int)i)
          state.selected_layer = -1;
        break;
      }
    }
  };
  lcb.on_layer_reorder = [&](const std::vector<std::string> &order) {
    yjs_op_layer_reorder(building.levels[state.level_idx].name, order);
  };
  layers_overlay_state_.selected_layer = state.selected_layer;
  canvas::draw_layers_overlay(building, state.level_idx,
                              state.floorplan_session, state.layer_session,
                              layers_overlay_state_, canvas_, lcb);
  state.selected_layer = layers_overlay_state_.selected_layer;

  ImGui::SetCursorScreenPos(canvas_.canvas_pos());
  ImGui::InvisibleButton("##canvas", canvas_.canvas_size());
  ImGuiIO &io = ImGui::GetIO();
  bool hovered = ImGui::IsItemHovered();
  ImVec2 mouse = io.MousePos;
  canvas_.handle_pan_zoom(hovered);

  auto world_to_screen = [&](double wx, double wy) {
    return canvas_.world_to_screen(wx, wy);
  };
  auto screen_to_world = [&](ImVec2 sp) { return canvas_.screen_to_world(sp); };

  auto hit_vertex = [&](ImVec2 m) -> int {
    int best = -1;
    float best_d = kHitRadiusPx;
    for (int i = 0; i < (int)level.vertices.size(); ++i) {
      ImVec2 p = world_to_screen(level.vertices[i].x, level.vertices[i].y);
      float dx = m.x - p.x, dy = m.y - p.y;
      float d = std::sqrt(dx * dx + dy * dy);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    return best;
  };
  auto hit_lane = [&](ImVec2 m) -> int {
    int best = -1;
    float best_d = kLaneHitPx;
    for (int i = 0; i < (int)level.lanes.size(); ++i) {
      const Lane &l = level.lanes[i];
      if (l.start_idx < 0 || l.start_idx >= (int)level.vertices.size() ||
          l.end_idx < 0 || l.end_idx >= (int)level.vertices.size())
        continue;
      ImVec2 a = world_to_screen(level.vertices[l.start_idx].x,
                                 level.vertices[l.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[l.end_idx].x,
                                 level.vertices[l.end_idx].y);
      float d = dist_point_segment(m.x, m.y, a.x, a.y, b.x, b.y);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    return best;
  };

  static bool s_dragging = false;
  static bool s_drag_moved_vertices = false;
  static ImVec2 s_mouse_down_screen{0, 0};
  static double s_drag_start_world_x = 0.0, s_drag_start_world_y = 0.0;
  static std::vector<std::pair<double, double>> s_drag_origins;

  constexpr float kClickThresholdPx = 4.0f;

  if (hovered && (state.mode == Mode::Vertex || state.mode == Mode::Lane)) {
    int hv = hit_vertex(mouse);
    ImDrawList *dl = canvas_.draw_list();
    if (state.mode == Mode::Lane && s_dragging) {
      float ddx = mouse.x - s_mouse_down_screen.x;
      float ddy = mouse.y - s_mouse_down_screen.y;
      if (std::sqrt(ddx * ddx + ddy * ddy) > kClickThresholdPx) {
        dl->AddLine(s_mouse_down_screen, mouse, IM_COL32(255, 90, 90, 200),
                    2.0f);
      }
    }
    if (state.mode == Mode::Lane && hv >= 0 && hv != state.pending_lane_start) {
      ImVec2 p =
          canvas_.world_to_screen(level.vertices[hv].x, level.vertices[hv].y);
      dl->AddCircle(p, 9.5f, IM_COL32(80, 170, 255, 255), 0, 2.5f);
    }
    ImVec2 ghost = mouse;
    if (state.mode == Mode::Lane && hv < 0 && state.pending_lane_start >= 0 &&
        state.pending_lane_start < (int)level.vertices.size() &&
        ImGui::GetIO().KeyShift) {
      const Vertex &sv = level.vertices[state.pending_lane_start];
      auto [wx, wy] = canvas_.screen_to_world(mouse);
      auto [sdx, sdy] = snap_axis_or_diagonal(wx - sv.x, wy - sv.y);
      ghost = canvas_.world_to_screen(sv.x + sdx, sv.y + sdy);
    }
    if (state.mode == Mode::Lane && state.pending_lane_start >= 0 &&
        state.pending_lane_start < (int)level.vertices.size()) {
      ImVec2 a =
          canvas_.world_to_screen(level.vertices[state.pending_lane_start].x,
                                  level.vertices[state.pending_lane_start].y);
      ImVec2 b = (hv >= 0) ? canvas_.world_to_screen(level.vertices[hv].x,
                                                     level.vertices[hv].y)
                           : ghost;
      dl->AddLine(a, b, IM_COL32(140, 220, 140, 130), 3.0f);
    }
    if (hv < 0) {
      dl->AddCircleFilled(ghost, 4.5f, IM_COL32(230, 230, 230, 110));
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    const float r = 9.0f;
    dl->AddLine(ImVec2(mouse.x - r, mouse.y), ImVec2(mouse.x + r, mouse.y),
                IM_COL32(255, 255, 255, 230), 1.5f);
    dl->AddLine(ImVec2(mouse.x, mouse.y - r), ImVec2(mouse.x, mouse.y + r),
                IM_COL32(255, 255, 255, 230), 1.5f);
    dl->AddLine(ImVec2(mouse.x - r, mouse.y), ImVec2(mouse.x + r, mouse.y),
                IM_COL32(0, 0, 0, 230), 0.5f);
    dl->AddLine(ImVec2(mouse.x, mouse.y - r), ImVec2(mouse.x, mouse.y + r),
                IM_COL32(0, 0, 0, 230), 0.5f);
  }

  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    s_dragging = true;
    s_mouse_down_screen = mouse;
    auto [wx, wy] = screen_to_world(mouse);
    s_drag_start_world_x = wx;
    s_drag_start_world_y = wy;
    s_drag_moved_vertices = false;
    marquee_active_ = false;
    s_drag_origins.clear();

    int vidx = hit_vertex(mouse);
    int lidx = (vidx < 0) ? hit_lane(mouse) : -1;

    const bool shift = ImGui::GetIO().KeyShift;
    auto toggle_lane = [&](int li) {
      auto it = std::find(state.selected_lanes.begin(),
                          state.selected_lanes.end(), li);
      if (it == state.selected_lanes.end())
        state.selected_lanes.push_back(li);
      else
        state.selected_lanes.erase(it);
    };
    switch (state.mode) {
    case Mode::Pan: {
      if (vidx >= 0) {
        if (shift) {
          if (is_selected(state.selected_vertices, vidx))
            remove_from_selection(state.selected_vertices, vidx);
          else
            add_to_selection(state.selected_vertices, vidx);
        } else {
          state.selected_lanes.clear();
          if (!is_selected(state.selected_vertices, vidx)) {
            state.selected_vertices.clear();
            state.selected_vertices.push_back(vidx);
          }
        }
      } else if (lidx >= 0) {
        if (shift) {
          toggle_lane(lidx);
        } else {
          state.selected_vertices.clear();
          state.selected_lanes.clear();
          state.selected_lanes.push_back(lidx);
        }
      } else {
        if (!shift) {
          state.selected_vertices.clear();
          state.selected_lanes.clear();
        }
        marquee_active_ = true;
        marquee_start_ = mouse;
      }
      break;
    }
    case Mode::Vertex: {
      if (vidx >= 0) {
        if (shift) {
          if (is_selected(state.selected_vertices, vidx)) {
            remove_from_selection(state.selected_vertices, vidx);
          } else {
            add_to_selection(state.selected_vertices, vidx);
          }
        } else if (!is_selected(state.selected_vertices, vidx)) {
          state.selected_vertices.clear();
          state.selected_vertices.push_back(vidx);
          state.selected_lanes.clear();
        }
        for (int i : state.selected_vertices) {
          s_drag_origins.push_back({level.vertices[i].x, level.vertices[i].y});
        }
      } else if (lidx >= 0) {
        if (shift) {
          toggle_lane(lidx);
        } else {
          state.selected_vertices.clear();
          state.selected_lanes.clear();
          state.selected_lanes.push_back(lidx);
        }
      } else {
        if (!shift) {
          state.selected_vertices.clear();
          state.selected_lanes.clear();
        }
        marquee_active_ = true;
        marquee_start_ = mouse;
      }
      break;
    }
    case Mode::Lane: {
      // Decided at release so we can tell click-to-chain from drag-to-bisect.
      break;
    }
    }
  }

  if (s_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (state.mode == Mode::Vertex && !s_drag_origins.empty()) {
      auto [wx, wy] = screen_to_world(mouse);
      double dx = wx - s_drag_start_world_x;
      double dy = wy - s_drag_start_world_y;
      if (ImGui::GetIO().KeyShift) {
        auto snapped = snap_axis_or_diagonal(dx, dy);
        dx = snapped.first;
        dy = snapped.second;
      }
      float sdx = mouse.x - s_mouse_down_screen.x;
      float sdy = mouse.y - s_mouse_down_screen.y;
      if (std::sqrt(sdx * sdx + sdy * sdy) > kClickThresholdPx) {
        s_drag_moved_vertices = true;
      }
      if (s_drag_moved_vertices) {
        for (size_t k = 0;
             k < state.selected_vertices.size() && k < s_drag_origins.size();
             ++k) {
          int vi = state.selected_vertices[k];
          if (vi >= 0 && vi < (int)level.vertices.size()) {
            level.vertices[vi].x = s_drag_origins[k].first + dx;
            level.vertices[vi].y = s_drag_origins[k].second + dy;
          }
        }
      }
    }
  }

  if (s_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (s_drag_moved_vertices) {
      for (int vi : state.selected_vertices) {
        if (vi >= 0 && vi < (int)level.vertices.size()) {
          yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
        }
      }
    }
    if (marquee_active_) {
      float sdx = mouse.x - s_mouse_down_screen.x;
      float sdy = mouse.y - s_mouse_down_screen.y;
      bool moved = std::sqrt(sdx * sdx + sdy * sdy) > kClickThresholdPx;
      if (moved) {
        ImVec2 a = marquee_start_, b = mouse;
        ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
        ImVec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
        if (!ImGui::GetIO().KeyShift) {
          state.selected_vertices.clear();
          state.selected_lanes.clear();
        }
        for (int i = 0; i < (int)level.vertices.size(); ++i) {
          ImVec2 p = world_to_screen(level.vertices[i].x, level.vertices[i].y);
          if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y) {
            add_to_selection(state.selected_vertices, i);
          }
        }
        for (int i = 0; i < (int)level.lanes.size(); ++i) {
          const Lane &ln = level.lanes[i];
          if (ln.start_idx < 0 || ln.start_idx >= (int)level.vertices.size() ||
              ln.end_idx < 0 || ln.end_idx >= (int)level.vertices.size())
            continue;
          ImVec2 pa = world_to_screen(level.vertices[ln.start_idx].x,
                                      level.vertices[ln.start_idx].y);
          ImVec2 pb = world_to_screen(level.vertices[ln.end_idx].x,
                                      level.vertices[ln.end_idx].y);
          bool ain =
              pa.x >= lo.x && pa.x <= hi.x && pa.y >= lo.y && pa.y <= hi.y;
          bool bin =
              pb.x >= lo.x && pb.x <= hi.x && pb.y >= lo.y && pb.y <= hi.y;
          if (ain && bin &&
              std::find(state.selected_lanes.begin(),
                        state.selected_lanes.end(),
                        i) == state.selected_lanes.end())
            state.selected_lanes.push_back(i);
        }
      } else if (state.mode == Mode::Vertex) {
        Vertex v;
        v.x = s_drag_start_world_x;
        v.y = s_drag_start_world_y;
        yjs_op_vertex_add(level.name, v);
        level.vertices.push_back(v);
        state.selected_vertices.clear();
        state.selected_vertices.push_back((int)level.vertices.size() - 1);
      }
    }
    if (state.mode == Mode::Lane && !marquee_active_) {
      float sdx = mouse.x - s_mouse_down_screen.x;
      float sdy = mouse.y - s_mouse_down_screen.y;
      bool drag = std::sqrt(sdx * sdx + sdy * sdy) > kClickThresholdPx;
      if (drag) {
        auto [end_wx, end_wy] = screen_to_world(mouse);
        int original_count = (int)level.lanes.size();
        std::vector<std::tuple<int, double, double>> splits;
        for (int i = 0; i < original_count; ++i) {
          const Lane &ln = level.lanes[i];
          if (ln.start_idx < 0 || ln.start_idx >= (int)level.vertices.size() ||
              ln.end_idx < 0 || ln.end_idx >= (int)level.vertices.size())
            continue;
          const Vertex &va = level.vertices[ln.start_idx];
          const Vertex &vb = level.vertices[ln.end_idx];
          double ix = 0, iy = 0;
          if (segment_intersect(s_drag_start_world_x, s_drag_start_world_y,
                                end_wx, end_wy, va.x, va.y, vb.x, vb.y, ix,
                                iy)) {
            splits.emplace_back(i, ix, iy);
          }
        }
        std::sort(splits.begin(), splits.end(),
                  [](const auto &a, const auto &b) {
                    return std::get<0>(a) > std::get<0>(b);
                  });
        for (auto &[li, ix, iy] : splits) {
          Lane orig = level.lanes[li];
          Vertex nv;
          nv.x = ix;
          nv.y = iy;
          yjs_op_vertex_add(level.name, nv);
          level.vertices.push_back(nv);
          int new_vi = (int)level.vertices.size() - 1;
          yjs_op_lane_delete(level.name, li);
          delete_lane(level, li);
          Lane a = orig;
          a.end_idx = new_vi;
          yjs_op_lane_add(level.name, a);
          level.lanes.push_back(a);
          Lane b = orig;
          b.start_idx = new_vi;
          yjs_op_lane_add(level.name, b);
          level.lanes.push_back(b);
        }
        state.pending_lane_start = -1;
        state.selected_lanes.clear();
        state.selected_vertices.clear();
      } else {
        int vidx_r = hit_vertex(mouse);
        int lidx_r = (vidx_r < 0) ? hit_lane(mouse) : -1;
        auto extend_chain = [&](int vi) {
          if (state.pending_lane_start < 0) {
            state.pending_lane_start = vi;
            state.selected_vertices = {vi};
            state.selected_lanes.clear();
            return;
          }
          if (state.pending_lane_start == vi)
            return;
          Lane l;
          l.start_idx = state.pending_lane_start;
          l.end_idx = vi;
          init_default_lane_params(l);
          yjs_op_lane_add(level.name, l);
          level.lanes.push_back(l);
          state.pending_lane_start = vi;
          state.selected_vertices = {vi};
          state.selected_lanes.clear();
        };
        if (vidx_r >= 0) {
          extend_chain(vidx_r);
        } else if (lidx_r >= 0 && state.pending_lane_start < 0) {
          state.selected_lanes = {lidx_r};
          state.selected_vertices.clear();
        } else {
          Vertex v;
          v.x = s_drag_start_world_x;
          v.y = s_drag_start_world_y;
          if (ImGui::GetIO().KeyShift && state.pending_lane_start >= 0 &&
              state.pending_lane_start < (int)level.vertices.size()) {
            const Vertex &sv = level.vertices[state.pending_lane_start];
            auto [snx, sny] = snap_axis_or_diagonal(v.x - sv.x, v.y - sv.y);
            v.x = sv.x + snx;
            v.y = sv.y + sny;
          }
          yjs_op_vertex_add(level.name, v);
          level.vertices.push_back(v);
          extend_chain((int)level.vertices.size() - 1);
        }
      }
    }
    s_dragging = false;
    marquee_active_ = false;
    s_drag_moved_vertices = false;
    s_drag_origins.clear();
  }
}

void EditorView::draw_add_layer_section(Building &building,
                                        EditorState &state) {
  Level &level = building.levels[state.level_idx];
  double mpp = compute_level_mpp(building, state.level_idx);

  if (ImGui::Button("+ Add Layer")) {
    state.open_add_layer_modal = true;
    state.new_layer_name.clear();
    state.new_layer_filename.clear();
  }
  ImGui::SameLine();
  if (mpp > 0.0)
    ImGui::TextDisabled("mpp=%.4f m/px", mpp);
  else
    ImGui::TextDisabled("no mpp set");

  if (state.open_add_layer_modal) {
    ImGui::OpenPopup("Add Layer");
    state.open_add_layer_modal = false;
  }
  if (ImGui::BeginPopupModal("Add Layer", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::InputText("Name", &state.new_layer_name);
    ImGui::InputText("Filename", &state.new_layer_filename);
    ImGui::TextDisabled(
        "Path is resolved relative to the building.yaml directory.\n"
        "Use ../ to reference siblings (e.g. ../../dnd_ros2/...).");
    ImGui::Separator();
    bool can_add =
        !state.new_layer_name.empty() && !state.new_layer_filename.empty();
    if (!can_add)
      ImGui::BeginDisabled();
    if (ImGui::Button("Add", ImVec2(120, 0))) {
      Layer L;
      L.name = state.new_layer_name;
      L.filename = state.new_layer_filename;
      L.visible = true;
      L.color_r = 1.0;
      L.color_g = 1.0;
      L.color_b = 1.0;
      L.color_a = 0.5;
      L.scale = (mpp > 0.0) ? mpp : 1.0;
      L.yaw = 0.0;
      L.translation_x = 0.0;
      L.translation_y = 0.0;
      yjs_op_layer_set(level.name, L);
      level.layers.insert(level.layers.begin(), L);
      state.selected_layer = 0;
      ImGui::CloseCurrentPopup();
    }
    if (!can_add)
      ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void EditorView::draw_attribute_panel(Building &building, EditorState &state) {
  Level &level = building.levels[state.level_idx];

  bool is_layer_branch = (state.selected_layer >= 0 &&
                          state.selected_layer < (int)level.layers.size());
  int single_vertex =
      (state.selected_vertices.size() == 1 && state.selected_vertices[0] >= 0 &&
       state.selected_vertices[0] < (int)level.vertices.size())
          ? state.selected_vertices[0]
          : -1;
  int single_lane =
      (state.selected_lanes.size() == 1 && state.selected_lanes[0] >= 0 &&
       state.selected_lanes[0] < (int)level.lanes.size())
          ? state.selected_lanes[0]
          : -1;
  if (is_layer_branch) {
    flush_pending_vertex(level, state);
    flush_pending_lane(level, state);
    const std::string &cur = level.layers[state.selected_layer].name;
    if (!state.pending_commit_layer.empty() &&
        state.pending_commit_layer != cur)
      flush_pending_layer(level, state);
  } else {
    flush_pending_layer(level, state);
    if (state.pending_commit_vertex >= 0 &&
        state.pending_commit_vertex != single_vertex)
      flush_pending_vertex(level, state);
    if (state.pending_commit_lane >= 0 &&
        state.pending_commit_lane != single_lane)
      flush_pending_lane(level, state);
  }

  // Layer transform editor takes precedence when a layer is selected.
  if (is_layer_branch) {
    Layer &L = level.layers[state.selected_layer];
    const std::string layer_name = L.name;
    ImGui::Text("Layer: %s", L.name.c_str());
    ImGui::TextDisabled("filename: %s", L.filename.c_str());
    ImGui::Separator();
    ImGui::TextDisabled("Transform (rmf semantics: meters / radians)");
    float scale = (float)L.scale;
    float yaw_deg = (float)(L.yaw * 180.0 / M_PI);
    float tx = (float)L.translation_x;
    float ty = (float)L.translation_y;
    bool l_dirty = false, l_commit = false;
    if (ImGui::DragFloat("scale (m/px)", &scale, 0.0005f, 0.0001f, 100.0f,
                         "%.6f")) {
      L.scale = scale;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::DragFloat("yaw (deg)", &yaw_deg, 0.5f, -360.0f, 360.0f,
                         "%.2f")) {
      L.yaw = yaw_deg * M_PI / 180.0;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::DragFloat("translation_x (m)", &tx, 0.5f)) {
      L.translation_x = tx;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::DragFloat("translation_y (m)", &ty, 0.5f)) {
      L.translation_y = ty;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    ImGui::Separator();
    ImGui::TextDisabled("Color (RGBA persisted)");
    float r = (float)L.color_r, g = (float)L.color_g, b = (float)L.color_b;
    if (ImGui::SliderFloat("r", &r, 0.0f, 1.0f)) {
      L.color_r = r;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::SliderFloat("g", &g, 0.0f, 1.0f)) {
      L.color_g = g;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::SliderFloat("b", &b, 0.0f, 1.0f)) {
      L.color_b = b;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    float a = (float)L.color_a;
    if (ImGui::SliderFloat("a", &a, 0.0f, 1.0f)) {
      L.color_a = a;
      l_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      l_commit = true;
    if (ImGui::Checkbox("visible", &L.visible))
      l_commit = true;
    if (l_dirty) {
      state.pending_commit_layer = layer_name;
      state.pending_commit_time = ImGui::GetTime();
    }
    if (l_commit) {
      yjs_op_layer_set(level.name, L);
      state.pending_commit_layer.clear();
      state.pending_commit_time = 0.0;
    }
    return;
  }

  bool emitted = false;
  if (state.selected_vertices.size() > 1) {
    ImGui::Text("Multi-select: %d vertices",
                (int)state.selected_vertices.size());
    ImGui::TextDisabled("Use Align H/V or Delete in the top bar.");
    ImGui::TextDisabled(
        "Shift+click to toggle. Shift while dragging snaps to axis.");
    emitted = true;
  } else if (single_vertex >= 0) {
    int vi = single_vertex;
    Vertex &v = level.vertices[vi];
    ImGui::Text("Vertex #%d", vi);
    ImGui::Separator();
    bool v_dirty = false, v_commit = false;
    float xf = (float)v.x, yf = (float)v.y;
    if (ImGui::InputFloat("x", &xf, 0.0f, 0.0f, "%.3f")) {
      v.x = xf;
      v_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      v_commit = true;
    if (ImGui::InputFloat("y", &yf, 0.0f, 0.0f, "%.3f")) {
      v.y = yf;
      v_dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      v_commit = true;
    if (ImGui::InputText("name", &v.name))
      v_dirty = true;
    if (ImGui::IsItemDeactivatedAfterEdit())
      v_commit = true;
    ImGui::Separator();
    ImGui::TextDisabled("Attributes");
    for (const auto &spec : kVertexParams) {
      draw_param_editor(v.params, spec.key, spec.type, v_dirty, v_commit);
    }
    if (v_dirty) {
      state.pending_commit_vertex = vi;
      state.pending_commit_time = ImGui::GetTime();
    }
    if (v_commit) {
      yjs_op_vertex_replace(level.name, vi, v);
      state.pending_commit_vertex = -1;
      state.pending_commit_time = 0.0;
    }
    emitted = true;
  }

  if (single_lane >= 0) {
    if (emitted)
      ImGui::Separator();
    Lane &l = level.lanes[single_lane];
    ImGui::Text("Lane #%d  (%d -> %d)", single_lane, l.start_idx, l.end_idx);
    ImGui::Separator();
    init_default_lane_params(l);
    bool ln_dirty = false, ln_commit = false;
    for (const auto &spec : kLaneParams) {
      if (std::strcmp(spec.key, "orientation") == 0)
        continue;
      draw_param_editor(l.params, spec.key, spec.type, ln_dirty, ln_commit);
    }
    draw_orientation_combo(l.params, ln_dirty, ln_commit);
    if (ln_dirty) {
      state.pending_commit_lane = single_lane;
      state.pending_commit_time = ImGui::GetTime();
    }
    if (ln_commit) {
      yjs_op_lane_replace(level.name, single_lane, l);
      state.pending_commit_lane = -1;
      state.pending_commit_time = 0.0;
    }
    emitted = true;
  } else if (state.selected_lanes.size() >= 2) {
    std::vector<int> sel;
    for (int li : state.selected_lanes)
      if (li >= 0 && li < (int)level.lanes.size())
        sel.push_back(li);
    if (!sel.empty()) {
      if (emitted)
        ImGui::Separator();
      ImGui::Text("Multi-select: %d lanes", (int)sel.size());
      ImGui::Separator();

      auto bi_of = [&](int li) -> int {
        auto &p = level.lanes[li].params;
        auto it = p.find("bidirectional");
        if (it == p.end() || it->second.type != ParamType::BOOL)
          return 1;
        return it->second.b ? 1 : 0;
      };
      auto orient_of = [&](int li) -> int {
        auto &p = level.lanes[li].params;
        auto it = p.find("orientation");
        if (it == p.end() || it->second.type != ParamType::STRING)
          return 0;
        if (it->second.s == "forward")
          return 1;
        if (it->second.s == "backward")
          return 2;
        return 0;
      };
      int bi_common = bi_of(sel[0]);
      int orient_common = orient_of(sel[0]);
      bool bi_mixed = false, orient_mixed = false;
      for (size_t i = 1; i < sel.size(); ++i) {
        if (bi_of(sel[i]) != bi_common)
          bi_mixed = true;
        if (orient_of(sel[i]) != orient_common)
          orient_mixed = true;
      }

      auto commit_all = [&]() {
        for (int li : sel)
          yjs_op_lane_replace(level.name, li, level.lanes[li]);
      };

      bool vb = bi_common != 0;
      ImGui::PushID("multi_bi");
      if (bi_mixed) {
        const char *opts[] = {"(mixed)", "false", "true"};
        int cur = 0;
        if (ImGui::Combo("bidirectional", &cur, opts, 3) && cur != 0) {
          for (int li : sel)
            level.lanes[li].params["bidirectional"] =
                ParamValue::make_bool(cur == 2);
          commit_all();
        }
      } else {
        if (ImGui::Checkbox("bidirectional", &vb)) {
          for (int li : sel)
            level.lanes[li].params["bidirectional"] = ParamValue::make_bool(vb);
          commit_all();
        }
      }
      ImGui::PopID();

      ImGui::PushID("multi_orient");
      if (orient_mixed) {
        const char *opts[] = {"(mixed)", "(none)", "forward", "backward"};
        int cur = 0;
        if (ImGui::Combo("orientation", &cur, opts, 4) && cur != 0) {
          std::string val = (cur == 2)   ? "forward"
                            : (cur == 3) ? "backward"
                                         : "";
          for (int li : sel)
            level.lanes[li].params["orientation"] =
                ParamValue::make_string(val);
          commit_all();
        }
      } else {
        const char *opts[] = {"(none)", "forward", "backward"};
        int cur = orient_common;
        if (ImGui::Combo("orientation", &cur, opts, 3)) {
          std::string val = (cur == 1)   ? "forward"
                            : (cur == 2) ? "backward"
                                         : "";
          for (int li : sel)
            level.lanes[li].params["orientation"] =
                ParamValue::make_string(val);
          commit_all();
        }
      }
      ImGui::PopID();
      emitted = true;
    }
  }

  if (!emitted) {
    ImGui::TextDisabled("No selection.");
  }
}

void EditorView::draw_status_bar(const EditorState &state) {
  const char *save_label = "";
  ImU32 col = IM_COL32(180, 180, 180, 255);
  switch (state.save_state) {
  case SaveState::Idle:
    save_label = "idle";
    break;
  case SaveState::Saving:
    save_label = "saving…";
    col = IM_COL32(220, 200, 100, 255);
    break;
  case SaveState::Saved:
    save_label = "saved";
    col = IM_COL32(100, 220, 120, 255);
    break;
  case SaveState::Conflict:
    save_label = "conflict";
    col = IM_COL32(255, 120, 120, 255);
    break;
  case SaveState::BadRequest:
    save_label = "bad yaml";
    col = IM_COL32(255, 120, 120, 255);
    break;
  case SaveState::NetworkError:
    save_label = "net error";
    col = IM_COL32(255, 120, 120, 255);
    break;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::Text("%s", save_label);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("| etag: %s",
                      state.etag.empty() ? "-" : state.etag.c_str());

#ifdef __EMSCRIPTEN__
  const char *yjs = map_editor_yjs_status();
  if (yjs && yjs[0]) {
    bool ok = std::strcmp(yjs, "connected") == 0 && map_editor_yjs_synced();
    ImU32 ycol = ok ? IM_COL32(100, 220, 120, 255)
                    : (std::strcmp(yjs, "connecting") == 0
                           ? IM_COL32(220, 200, 100, 255)
                           : IM_COL32(255, 120, 120, 255));
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ycol);
    ImGui::Text("yjs: %s%s", yjs, ok ? " (synced)" : "");
    ImGui::PopStyleColor();
    std::free((void *)yjs);
  } else if (yjs) {
    std::free((void *)yjs);
  }
#endif

  if (!state.status_message.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", state.status_message.c_str());
  }
}

} // namespace imrmf::map_editor
