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
#include <ctime>
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

bool g_readonly = false;

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
void mevjs_fiducial_add(const char *level, const char *yaml);
void mevjs_fiducial_replace(const char *level, int idx, const char *yaml);
void mevjs_fiducial_delete(const char *level, int idx);
void mevjs_set_reference_level(const char *name);
}
#endif

void yjs_op_vertex_add(const std::string &level, const Vertex &v) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  std::string y = serialize_vertex(v);
  mevjs_vertex_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)v;
#endif
}
void yjs_op_vertex_replace(const std::string &level, int idx, const Vertex &v) {
  if (g_readonly) return;
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
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  mevjs_vertex_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_lane_add(const std::string &level, const Lane &l) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  std::string y = serialize_lane(l);
  mevjs_lane_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)l;
#endif
}
void yjs_op_lane_replace(const std::string &level, int idx, const Lane &l) {
  if (g_readonly) return;
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
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  mevjs_lane_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_layer_set(const std::string &level, const Layer &L) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  std::string y = serialize_layer(L);
  mevjs_layer_set(level.c_str(), L.name.c_str(), y.c_str());
#else
  (void)level;
  (void)L;
#endif
}
void yjs_op_layer_delete(const std::string &level, const std::string &name) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  mevjs_layer_delete(level.c_str(), name.c_str());
#else
  (void)level;
  (void)name;
#endif
}
void yjs_op_fiducial_add(const std::string &level, const Fiducial &f) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  std::string y = serialize_fiducial(f);
  mevjs_fiducial_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)f;
#endif
}
void yjs_op_fiducial_replace(const std::string &level, int idx,
                             const Fiducial &f) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  std::string y = serialize_fiducial(f);
  mevjs_fiducial_replace(level.c_str(), idx, y.c_str());
#else
  (void)level;
  (void)idx;
  (void)f;
#endif
}
void yjs_op_fiducial_delete(const std::string &level, int idx) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  mevjs_fiducial_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_set_reference_level(const std::string &name) {
  if (g_readonly) return;
#ifdef __EMSCRIPTEN__
  mevjs_set_reference_level(name.c_str());
#else
  (void)name;
#endif
}

void yjs_op_layer_reorder(const std::string &level,
                          const std::vector<std::string> &names) {
  if (g_readonly) return;
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
EM_JS(void, mevjs_fiducial_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.fiducialAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_fiducial_replace,
      (const char *level, int idx, const char *yaml), {
        if (window.imrmf.yjs)
          window.imrmf.yjs.fiducialReplace(UTF8ToString(level), idx,
                                           UTF8ToString(yaml));
      });
EM_JS(void, mevjs_fiducial_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.fiducialDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_set_reference_level, (const char *name), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.setReferenceLevelName(UTF8ToString(name));
});

#endif // __EMSCRIPTEN__

} // namespace

EditorView::EditorView(std::string /*image_root_unused*/,
                       std::string building_id)
    : building_id_(std::move(building_id)),
      texture_provider_(std::make_unique<canvas::HttpTextureProvider>()),
      canvas_(building_id_, texture_provider_.get()) {}

EditorView::~EditorView() = default;

void EditorView::apply_snapshot_dir(const std::string &dir) {
  auto enc = [](const std::string &s) {
    std::string o;
    o.reserve(s.size() * 3);
    char buf[4];
    for (unsigned char c : s) {
      bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                        c == '.' || c == '~';
      if (unreserved) {
        o.push_back((char)c);
      } else {
        std::snprintf(buf, sizeof(buf), "%%%02X", c);
        o.append(buf);
      }
    }
    return o;
  };
  if (dir.empty()) {
    texture_provider_->set_url_builder(
        [enc](const std::string &id, const std::string &path) {
          return "/layer_asset?id=" + enc(id) + "&path=" + enc(path);
        });
  } else {
    std::string d = dir;
    texture_provider_->set_url_builder(
        [enc, d](const std::string &id, const std::string &path) {
          return "/snapshot_asset?id=" + enc(id) + "&dir=" + enc(d) +
                 "&path=" + enc(path);
        });
  }
  texture_provider_->clear_cache();
}

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
  {
    const float version_h = 32.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas_box", ImVec2(avail.x, avail.y - version_h), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    draw_canvas(building, state);
    ImGui::EndChild();
    draw_version_strip(state);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("right_col", ImVec2(right_col_w, region.y - status_h),
                    false);
  if (state.align_floors_mode) {
    draw_align_floors_panel(building, state);
  } else {
    draw_building_panel(building, state);
    ImGui::Separator();
    ImGui::TextDisabled("View");
    ImGui::Checkbox("Show fiducials", &state.show_fiducials);
    if (!state.show_fiducials) state.selected_fiducial_idx = -1;
    ImGui::Separator();
    draw_add_layer_section(building, state);
    ImGui::Separator();
    draw_layer_config_panel(building, state);
    ImGui::Separator();
    draw_attribute_panel(building, state);
  }
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

  {
    const bool active = state.align_floors_mode;
    if (active)
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 130, 200, 255));
    if (ImGui::Button(active ? "Align floors [on]" : "Align floors")) {
      state.align_floors_mode = !state.align_floors_mode;
      if (state.align_floors_mode) {
        state.selected_vertices.clear();
        state.selected_lanes.clear();
        state.selected_layer = -1;
        state.align_layer_idx = -1;
        state.pending_lane_start = -1;
        state.align_floors_sel_level = -1;
        state.align_floors_sel_idx = -1;
        if (state.align_floors_target < 0 ||
            state.align_floors_target == 0 ||
            state.align_floors_target >= (int)building.levels.size()) {
          state.align_floors_target = building.levels.size() >= 2 ? 1 : -1;
        }
        int max_id = 0;
        for (const Level &lvl : building.levels) {
          for (const Fiducial &f : lvl.fiducials) {
            if (f.name.size() > 1 && f.name[0] == 'F') {
              try { max_id = std::max(max_id, std::stoi(f.name.substr(1))); }
              catch (...) {}
            }
          }
        }
        state.align_floors_next_id = max_id + 1;
        state.align_floors_next_name =
            "F" + std::to_string(state.align_floors_next_id);
        state.align_floors_placing = false;
        state.align_floors_image.clear();
        state.align_floors_ref_mpp = compute_level_mpp(building, 0);
        state.align_floors_tgt_mpp =
            (state.align_floors_target > 0)
                ? compute_level_mpp(building, state.align_floors_target)
                : 0.0;
      }
    }
    if (active)
      ImGui::PopStyleColor();
    ImGui::SameLine();
  }

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
  if (state.align_floors_mode) {
    draw_align_floors_canvas(building, state);
    return;
  }
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
    if (state.show_fiducials) {
      ImDrawList *dl = c.draw_list();
      for (int i = 0; i < (int)level.fiducials.size(); ++i) {
        const Fiducial &f = level.fiducials[i];
        ImVec2 p = c.world_to_screen(f.x, f.y);
        bool sel = (state.selected_fiducial_idx == i);
        const float r = sel ? 8.0f : 6.0f;
        ImU32 col = IM_COL32(110, 220, 120, 255);
        ImVec2 a(p.x, p.y - r), b(p.x + r, p.y);
        ImVec2 d(p.x, p.y + r), e(p.x - r, p.y);
        dl->AddQuadFilled(a, b, d, e, col);
        dl->AddQuad(a, b, d, e,
                    sel ? IM_COL32(255, 255, 255, 255)
                        : IM_COL32(20, 20, 20, 220),
                    sel ? 2.5f : 1.5f);
        if (!f.name.empty())
          dl->AddText(ImVec2(p.x + 8.0f, p.y - 8.0f), col, f.name.c_str());
      }
    }
  };
  canvas_.draw(building, state.level_idx, opts);
  canvas::draw_mouse_coord_hud(canvas_, building, state.level_idx);

  int new_idx = state.level_idx;
  if (canvas::draw_level_selector_overlay(building, new_idx, canvas_) &&
      new_idx != state.level_idx) {
    flush_all_pending(building.levels[state.level_idx], state);
    state.level_idx = new_idx;
    state.selected_vertices.clear();
    state.selected_lanes.clear();
    state.pending_lane_start = -1;
    state.selected_layer = -1;
    state.align_layer_idx = -1;
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
        else if (state.selected_layer > (int)i)
          state.selected_layer -= 1;
        if (state.align_layer_idx == (int)i)
          state.align_layer_idx = -1;
        else if (state.align_layer_idx > (int)i)
          state.align_layer_idx -= 1;
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

  // Direct-map layer align bypasses the regular tool pipeline.
  if (state.align_layer_idx >= 0 &&
      state.align_layer_idx < (int)level.layers.size()) {
    handle_align_input(building, state, hovered);
    return;
  }
  canvas_.handle_pan_zoom(hovered);

  auto world_to_screen = [&](double wx, double wy) {
    return canvas_.world_to_screen(wx, wy);
  };
  auto screen_to_world = [&](ImVec2 sp) { return canvas_.screen_to_world(sp); };

  static bool s_fid_dragging = false;
  static bool s_fid_moved = false;
  if (state.show_fiducials) {
    auto hit_fid = [&](ImVec2 m) -> int {
      int best = -1;
      float best_d = 9.0f;
      for (int i = 0; i < (int)level.fiducials.size(); ++i) {
        ImVec2 p = world_to_screen(level.fiducials[i].x, level.fiducials[i].y);
        float dx = m.x - p.x, dy = m.y - p.y;
        float d = std::sqrt(dx * dx + dy * dy);
        if (d < best_d) { best_d = d; best = i; }
      }
      return best;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      int hi = hit_fid(mouse);
      if (hi >= 0) {
        state.selected_fiducial_idx = hi;
        s_fid_dragging = true;
        s_fid_moved = false;
      }
    }
    if (s_fid_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        state.selected_fiducial_idx >= 0 &&
        state.selected_fiducial_idx < (int)level.fiducials.size()) {
      if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
        auto [wx, wy] = canvas_.screen_to_world(mouse);
        Fiducial &f = level.fiducials[state.selected_fiducial_idx];
        f.x = wx;
        f.y = wy;
        s_fid_moved = true;
      }
      return;
    }
    if (s_fid_dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (s_fid_moved && state.selected_fiducial_idx >= 0 &&
          state.selected_fiducial_idx < (int)level.fiducials.size()) {
        yjs_op_fiducial_replace(level.name, state.selected_fiducial_idx,
                                level.fiducials[state.selected_fiducial_idx]);
      }
      s_fid_dragging = false;
      s_fid_moved = false;
      return;
    }
    if (state.selected_fiducial_idx >= 0 &&
        state.selected_fiducial_idx < (int)level.fiducials.size() &&
        !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
      yjs_op_fiducial_delete(level.name, state.selected_fiducial_idx);
      level.fiducials.erase(level.fiducials.begin() +
                            state.selected_fiducial_idx);
      state.selected_fiducial_idx = -1;
    }
  }

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
    if ((state.mode == Mode::Vertex || state.mode == Mode::Pan) &&
        !s_drag_origins.empty()) {
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

void EditorView::draw_building_panel(Building &building, EditorState &state) {
  (void)state;
  ImGui::TextDisabled("Building");
  const std::string &cur = building.reference_level_name;
  const char *preview = !cur.empty()        ? cur.c_str()
                        : building.levels.empty() ? ""
                                            : building.levels.front().name.c_str();
  ImGui::Text("Reference level");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##reference_level", preview)) {
    for (const Level &lvl : building.levels) {
      bool sel = (lvl.name == cur);
      if (ImGui::Selectable(lvl.name.c_str(), sel) && lvl.name != cur) {
        building.reference_level_name = lvl.name;
        yjs_op_set_reference_level(lvl.name);
      }
      if (sel)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

void EditorView::draw_layer_config_panel(Building &building,
                                         EditorState &state) {
  Level &level = building.levels[state.level_idx];

  // Drop stale align target if a peer deleted the layer.
  if (state.align_layer_idx >= (int)level.layers.size())
    state.align_layer_idx = -1;

  ImGui::TextDisabled("Layer configuration");

  {
    ImGui::PushID("__cfg_fp");
    if (ImGui::CollapsingHeader("Floorplan (reference)",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      FloorplanSession &fps = state.floorplan_session[level.name];
      ImGui::Checkbox("visible##fp", &fps.visible);
      ImGui::SameLine();
      ImGui::Checkbox("invert##fp", &fps.invert);
      ImGui::SetNextItemWidth(-1);
      ImGui::SliderFloat("##fp_alpha", &fps.alpha, 0.0f, 1.0f, "alpha %.2f");
    }
    ImGui::PopID();
  }

  for (int i = 0; i < (int)level.layers.size(); ++i) {
    Layer &L = level.layers[i];
    const std::string layer_name = L.name;
    LayerSession &sess = state.layer_session[level.name + ":" + L.name];

    ImGui::PushID(i);

    const bool is_aligning = (state.align_layer_idx == i);
    if (is_aligning) {
      ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(50, 110, 50, 230));
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(60, 130, 60, 230));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(70, 150, 70, 255));
    }
    bool open = ImGui::CollapsingHeader(L.name.c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen);
    if (is_aligning)
      ImGui::PopStyleColor(3);
    if (!open) {
      ImGui::PopID();
      continue;
    }

    bool dirty = false, commit = false;

    if (ImGui::Checkbox("visible", &L.visible))
      commit = true;
    ImGui::SameLine();
    ImGui::Checkbox("invert", &sess.invert);

    float col[3] = {(float)L.color_r, (float)L.color_g, (float)L.color_b};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::ColorEdit3("##cfg_color", col, ImGuiColorEditFlags_NoInputs)) {
      L.color_r = col[0];
      L.color_g = col[1];
      L.color_b = col[2];
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;

    float a = (float)L.color_a;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##cfg_alpha", &a, 0.0f, 1.0f, "alpha %.2f")) {
      L.color_a = a;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;

    float scale = (float)L.scale;
    float yaw_deg = (float)(L.yaw * 180.0 / M_PI);
    float tx = (float)L.translation_x;
    float ty = (float)L.translation_y;
    if (ImGui::DragFloat("scale (m/px)", &scale, 0.0005f, 0.0001f, 100.0f,
                         "%.6f")) {
      L.scale = scale;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    if (ImGui::DragFloat("yaw (deg)", &yaw_deg, 0.5f, -360.0f, 360.0f,
                         "%.2f")) {
      L.yaw = yaw_deg * M_PI / 180.0;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    if (ImGui::DragFloat("tx (m)", &tx, 0.05f)) {
      L.translation_x = tx;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    if (ImGui::DragFloat("ty (m)", &ty, 0.05f)) {
      L.translation_y = ty;
      dirty = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;

    const char *btn = is_aligning ? "Stop aligning" : "Align on map";
    if (is_aligning) {
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 130, 60, 220));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 150, 70, 240));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(80, 170, 80, 255));
    }
    if (ImGui::Button(btn)) {
      flush_pending_layer(level, state);
      state.align_layer_idx = is_aligning ? -1 : i;
    }
    if (is_aligning)
      ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Wheel = scale, left-drag = translate.\n"
                        "Ctrl = fine (independent of canvas zoom).\n"
                        "Middle-drag still pans the view.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete##cfg")) {
#ifdef __EMSCRIPTEN__
      yjs_op_layer_delete(level.name, L.name);
#endif
      level.layers.erase(level.layers.begin() + i);
      if (state.selected_layer == i)
        state.selected_layer = -1;
      else if (state.selected_layer > i)
        state.selected_layer -= 1;
      if (state.align_layer_idx == i)
        state.align_layer_idx = -1;
      else if (state.align_layer_idx > i)
        state.align_layer_idx -= 1;
      ImGui::PopID();
      return; // iterator invalidated
    }

    if (dirty) {
      state.pending_commit_layer = layer_name;
      state.pending_commit_time = ImGui::GetTime();
    }
    if (commit) {
#ifdef __EMSCRIPTEN__
      yjs_op_layer_set(level.name, L);
#endif
      state.pending_commit_layer.clear();
      state.pending_commit_time = 0.0;
    }

    ImGui::PopID();
  }

  if (state.align_layer_idx >= 0 &&
      state.align_layer_idx < (int)level.layers.size()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Aligning: %s",
                       level.layers[state.align_layer_idx].name.c_str());
    ImGui::TextDisabled("Wheel = scale  /  drag = translate");
    ImGui::TextDisabled("Hold Ctrl for fine steps");
  }
}

void EditorView::handle_align_input(Building &building, EditorState &state,
                                    bool hovered) {
  Level &level = building.levels[state.level_idx];
  if (state.align_layer_idx < 0 ||
      state.align_layer_idx >= (int)level.layers.size())
    return;
  Layer &L = level.layers[state.align_layer_idx];
  ImGuiIO &io = ImGui::GetIO();

  if (io.KeyAlt) {
    canvas_.handle_pan_zoom(hovered);
    return;
  }

  double mpp = compute_level_mpp(building, state.level_idx);
  double eff_mpp = mpp > 0.0 ? mpp : 1.0;

  // Middle-drag still pans the view.
  static bool s_align_mpan = false;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    s_align_mpan = true;
  if (s_align_mpan) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      canvas_.view_state().offset_x += io.MouseDelta.x;
      canvas_.view_state().offset_y += io.MouseDelta.y;
    } else {
      s_align_mpan = false;
    }
  }

  bool dirty = false;

  // Wheel = scale. Ctrl = fine, zoom-independent.
  if (hovered && io.MouseWheel != 0.0f) {
    if (io.KeyCtrl) {
      double next = L.scale + (double)io.MouseWheel * 1e-4;
      L.scale = std::max(1e-6, next);
    } else {
      double factor = 1.0 + (double)io.MouseWheel * 0.02;
      L.scale = std::max(1e-6, L.scale * factor);
    }
    dirty = true;
  }

  // Left-drag = translation in meters. No Ctrl: layer follows cursor.
  // Ctrl: fixed small step per screen px, zoom-independent.
  static bool s_align_dragging = false;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    s_align_dragging = true;
  if (s_align_dragging) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const float dx = io.MouseDelta.x, dy = io.MouseDelta.y;
      if (dx != 0.0f || dy != 0.0f) {
        const float vs = canvas_.view_state().scale;
        double mx, my;
        if (io.KeyCtrl) {
          mx = (double)dx * eff_mpp * 0.01;
          my = (double)dy * eff_mpp * 0.01;
        } else {
          mx = (double)dx * eff_mpp / std::max(1e-6f, vs);
          my = (double)dy * eff_mpp / std::max(1e-6f, vs);
        }
        L.translation_x += mx;
        L.translation_y += my;
        dirty = true;
      }
    } else {
      s_align_dragging = false;
    }
  }

  if (dirty) {
    state.pending_commit_layer = L.name;
    flush_pending_layer(level, state);
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

namespace {

constexpr ImU32 kFidColorRef = IM_COL32(110, 220, 120, 255);
constexpr ImU32 kFidColorTgt = IM_COL32(255, 180, 80, 255);
constexpr ImU32 kFidColorSel = IM_COL32(255, 255, 255, 255);
constexpr float kFidHitPx = 9.0f;


void draw_fid_marker(ImDrawList *dl, ImVec2 p, const char *name, ImU32 col,
                     bool selected) {
  const float r = selected ? 8.0f : 6.0f;
  ImVec2 a(p.x, p.y - r), b(p.x + r, p.y);
  ImVec2 c(p.x, p.y + r), d(p.x - r, p.y);
  dl->AddQuadFilled(a, b, c, d, col);
  dl->AddQuad(a, b, c, d, selected ? kFidColorSel : IM_COL32(20, 20, 20, 220),
              selected ? 2.5f : 1.5f);
  if (name && *name)
    dl->AddText(ImVec2(p.x + 8.0f, p.y - 8.0f), col, name);
}

double default_target_scale(const Building &building, int tgt_idx) {
  double mpp_ref = compute_level_mpp(building, 0);
  double mpp_tgt = compute_level_mpp(building, tgt_idx);
  return (mpp_ref > 0.0 && mpp_tgt > 0.0) ? (mpp_tgt / mpp_ref) : 1.0;
}


void render_align_image(const canvas::MapCanvas &c,
                        canvas::TextureProvider &provider,
                        const std::string &asset_id, const Level &lvl,
                        const std::string &image_name, double level_mpp,
                        const FloorTransform *xf, ImU32 tint) {
  ImDrawList *dl = c.draw_list();
  canvas::LayerTexture *tex = nullptr;
  double s_w = 0.0, cosL = 1.0, sinL = 0.0, lx = 0.0, ly = 0.0;
  int ow = 0, oh = 0;
  if (image_name.empty()) {
    if (lvl.drawing_filename.empty()) return;
    tex = &provider.acquire("fp:" + lvl.name, asset_id, lvl.drawing_filename,
                            1.0, 1.0, 1.0);
    s_w = 1.0;
  } else {
    const Layer *L = nullptr;
    for (const Layer &q : lvl.layers)
      if (q.name == image_name) { L = &q; break; }
    if (!L) return;
    tex = &provider.acquire("lay:" + lvl.name + ":" + L->name, asset_id,
                            L->filename, L->color_r, L->color_g, L->color_b);
    s_w = L->scale / level_mpp;
    cosL = std::cos(L->yaw);
    sinL = std::sin(L->yaw);
    lx = L->translation_x / level_mpp;
    ly = L->translation_y / level_mpp;
  }
  if (!tex || tex->status != canvas::LoadStatus::Ok) return;
  ow = tex->orig_width > 0 ? tex->orig_width : tex->width;
  oh = tex->orig_height > 0 ? tex->orig_height : tex->height;
  auto corner = [&](double ix, double iy) {
    double a = ix * s_w, b = iy * s_w;
    double wx = lx + cosL * a - sinL * b;
    double wy = ly + sinL * a + cosL * b;
    if (xf) { auto p = tgt_to_ref(*xf, wx, wy); wx = p.first; wy = p.second; }
    return c.world_to_screen(wx, wy);
  };
  dl->AddImageQuad((void *)(intptr_t)tex->id, corner(0.0, 0.0),
                   corner((double)ow, 0.0), corner((double)ow, (double)oh),
                   corner(0.0, (double)oh), ImVec2(0, 0), ImVec2(1, 0),
                   ImVec2(1, 1), ImVec2(0, 1), tint);
}

void draw_align_image_combo(const Building &building, std::string &selection) {
  ImGui::Text("Show on both floors:");
  ImGui::SetNextItemWidth(-1);
  const char *preview = selection.empty() ? "floorplan" : selection.c_str();
  if (ImGui::BeginCombo("##align_image", preview)) {
    if (ImGui::Selectable("floorplan", selection.empty()))
      selection.clear();
    std::vector<std::string> seen;
    for (const Level &lvl : building.levels) {
      for (const Layer &L : lvl.layers) {
        if (std::find(seen.begin(), seen.end(), L.name) != seen.end())
          continue;
        seen.push_back(L.name);
        if (ImGui::Selectable(L.name.c_str(), selection == L.name))
          selection = L.name;
      }
    }
    ImGui::EndCombo();
  }
}

} // namespace

void EditorView::draw_align_floors_panel(Building &building,
                                         EditorState &state) {
  if (building.levels.empty()) {
    ImGui::TextDisabled("No levels loaded.");
    return;
  }
  ImGui::TextDisabled("Align floors");
  ImGui::Separator();

  Level &ref = building.levels[0];
  ImGui::Text("Reference: %s", ref.name.c_str());

  if (building.levels.size() < 2) {
    ImGui::TextDisabled("Need at least 2 levels.");
    return;
  }

  int tgt = state.align_floors_target;
  if (tgt <= 0 || tgt >= (int)building.levels.size()) tgt = 1;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##align_target", building.levels[tgt].name.c_str())) {
    for (int i = 1; i < (int)building.levels.size(); ++i) {
      bool sel = (i == tgt);
      if (ImGui::Selectable(building.levels[i].name.c_str(), sel)) {
        if (i != tgt) {
          tgt = i;
          state.align_floors_sel_level = -1;
          state.align_floors_sel_idx = -1;
          state.align_floors_ref_mpp = compute_level_mpp(building, 0);
          state.align_floors_tgt_mpp = compute_level_mpp(building, tgt);
        }
      }
      if (sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  state.align_floors_target = tgt;
  Level &target = building.levels[tgt];

  ImGui::Spacing();
  ImGui::Text("New fiducial name:");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##align_next_name", &state.align_floors_next_name);
  if (!state.align_floors_placing) {
    if (ImGui::Button("Add fiducial"))
      state.align_floors_placing = true;
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                       "Click on map (Esc cancels)");
    ImGui::SameLine();
    if (ImGui::SmallButton("cancel"))
      state.align_floors_placing = false;
  }

  draw_align_image_combo(building, state.align_floors_image);

  FloorTransform xf = compute_floor_transform(
      ref.fiducials, target.fiducials, default_target_scale(building, tgt));

  ImGui::Separator();
  ImGui::Text("Matched pairs: %d", xf.matched);
  if (xf.matched == 0)
    ImGui::TextDisabled("Place matching-named fiducials to align.");
  else if (xf.matched == 1)
    ImGui::TextDisabled("translation only — scroll-zoom is a no-op until 2+ pairs");
  ImGui::TextDisabled("scale %.5f  yaw %.2f°  t (%.1f, %.1f)", xf.scale,
                      xf.yaw * 180.0 / M_PI, xf.tx, xf.ty);

  int del_level = -1, del_idx = -1;
  auto fid_list = [&](Level &lvl, int level_idx, ImVec4 col,
                      const char *label) {
    ImGui::Separator();
    ImGui::TextColored(col, "%s fiducials (%d)", label,
                       (int)lvl.fiducials.size());
    for (int i = 0; i < (int)lvl.fiducials.size(); ++i) {
      const Fiducial &f = lvl.fiducials[i];
      ImGui::PushID(level_idx * 10000 + i);
      if (ImGui::SmallButton("X")) {
        del_level = level_idx;
        del_idx = i;
      }
      ImGui::SameLine();
      bool sel = (state.align_floors_sel_level == level_idx &&
                  state.align_floors_sel_idx == i);
      ImGui::TextColored(sel ? ImVec4(1, 1, 1, 1) : col,
                         "%s  (%.0f, %.0f)", f.name.c_str(), f.x, f.y);
      ImGui::PopID();
    }
  };
  fid_list(ref, 0, ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Reference");
  fid_list(target, tgt, ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Target");
  if (del_level >= 0) {
    Level &L = building.levels[del_level];
    if (del_idx >= 0 && del_idx < (int)L.fiducials.size()) {
      yjs_op_fiducial_delete(L.name, del_idx);
      L.fiducials.erase(L.fiducials.begin() + del_idx);
      if (state.align_floors_sel_level == del_level) {
        if (state.align_floors_sel_idx == del_idx) {
          state.align_floors_sel_level = -1;
          state.align_floors_sel_idx = -1;
        } else if (state.align_floors_sel_idx > del_idx) {
          state.align_floors_sel_idx -= 1;
        }
      }
    }
  }

  ImGui::Separator();
  if (ImGui::Button("Stop alignment")) {
    state.align_floors_mode = false;
    state.align_floors_sel_level = -1;
    state.align_floors_sel_idx = -1;
  }
}

void EditorView::draw_align_floors_canvas(Building &building,
                                          EditorState &state) {
  if (building.levels.empty()) {
    ImGui::TextDisabled("No levels loaded.");
    return;
  }
  const int tgt_idx = state.align_floors_target;
  const bool has_target =
      (tgt_idx > 0 && tgt_idx < (int)building.levels.size());

  FloorTransform xf;
  if (state.align_floors_ref_mpp <= 0.0)
    state.align_floors_ref_mpp = compute_level_mpp(building, 0);
  double ref_mpp = state.align_floors_ref_mpp;
  if (ref_mpp <= 0.0) ref_mpp = 1.0;
  double tgt_mpp = ref_mpp;
  if (has_target) {
    xf = compute_floor_transform(
        building.levels[0].fiducials, building.levels[tgt_idx].fiducials,
        default_target_scale(building, tgt_idx));
    if (state.align_floors_tgt_mpp <= 0.0)
      state.align_floors_tgt_mpp = compute_level_mpp(building, tgt_idx);
    if (state.align_floors_tgt_mpp > 0.0) tgt_mpp = state.align_floors_tgt_mpp;
  }

  canvas::DrawOptions opts;
  opts.show_vertex_names = false;
  opts.draw_floorplan = false;
  opts.draw_layers = false;
  opts.draw_vertices = false;
  opts.draw_lanes = false;
  opts.after_draw = [&, xf, tgt_mpp, ref_mpp, has_target](
                       const canvas::MapCanvas &c) {
    render_align_image(c, *texture_provider_, building_id_, building.levels[0],
                       state.align_floors_image, ref_mpp, nullptr,
                       IM_COL32(255, 255, 255, 255));
    if (has_target) {
      render_align_image(c, *texture_provider_, building_id_,
                         building.levels[tgt_idx], state.align_floors_image,
                         tgt_mpp, &xf, IM_COL32(255, 220, 160, 200));
    }
    ImDrawList *dl = c.draw_list();
    const Level &ref = building.levels[0];
    for (int i = 0; i < (int)ref.fiducials.size(); ++i) {
      const Fiducial &f = ref.fiducials[i];
      bool sel = (state.align_floors_sel_level == 0 &&
                  state.align_floors_sel_idx == i);
      draw_fid_marker(dl, c.world_to_screen(f.x, f.y), f.name.c_str(),
                      kFidColorRef, sel);
    }
    if (has_target) {
      const Level &target = building.levels[tgt_idx];
      for (int i = 0; i < (int)target.fiducials.size(); ++i) {
        const Fiducial &f = target.fiducials[i];
        auto [wx, wy] = tgt_to_ref(xf, f.x, f.y);
        bool sel = (state.align_floors_sel_level == tgt_idx &&
                    state.align_floors_sel_idx == i);
        draw_fid_marker(dl, c.world_to_screen(wx, wy), f.name.c_str(),
                        kFidColorTgt, sel);
      }
    }
  };
  canvas_.draw(building, 0, opts);
  canvas::draw_mouse_coord_hud(canvas_, ref_mpp);

  int ignore_level = 0;
  canvas::draw_level_selector_overlay(building, ignore_level, canvas_);

  ImGui::SetCursorScreenPos(canvas_.canvas_pos());
  ImGui::InvisibleButton("##align_floors_canvas", canvas_.canvas_size());
  bool hovered = ImGui::IsItemHovered();
  handle_floor_align_input(building, state, hovered);
}

void EditorView::handle_floor_align_input(Building &building,
                                          EditorState &state, bool hovered) {
  if (!state.align_floors_mode) return;
  const int tgt = state.align_floors_target;
  if (tgt <= 0 || tgt >= (int)building.levels.size()) return;
  Level &ref = building.levels[0];
  Level &target = building.levels[tgt];
  ImGuiIO &io = ImGui::GetIO();
  ImVec2 mouse = io.MousePos;

  if (io.KeyAlt) {
    canvas_.handle_pan_zoom(hovered);
    return;
  }

  FloorTransform xf = compute_floor_transform(
      ref.fiducials, target.fiducials, default_target_scale(building, tgt));

  auto canvas_to_floor = [&](int level, double cx, double cy) {
    if (level == 0) return std::pair<double, double>{cx, cy};
    return ref_to_tgt(xf, cx, cy);
  };
  auto floor_to_canvas = [&](int level, double fx, double fy) {
    if (level == 0) return std::pair<double, double>{fx, fy};
    return tgt_to_ref(xf, fx, fy);
  };

  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape))
    state.align_floors_placing = false;

  static bool s_mpan = false;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) s_mpan = true;
  if (s_mpan) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      canvas_.view_state().offset_x += io.MouseDelta.x;
      canvas_.view_state().offset_y += io.MouseDelta.y;
    } else s_mpan = false;
  }

  if (hovered && io.MouseWheel != 0.0f && !target.fiducials.empty() &&
      xf.scale > 1e-12) {
    double factor = 1.0 - (double)io.MouseWheel * 0.05;
    if (factor < 0.1) factor = 0.1;
    auto [piv_w_x, piv_w_y] = canvas_.screen_to_world(mouse);
    // canvas -> target floorplan-px pivot through the full inverse chain.
    auto [piv_t_x, piv_t_y] = canvas_to_floor(tgt, piv_w_x, piv_w_y);
    for (Fiducial &f : target.fiducials) {
      f.x = piv_t_x + factor * (f.x - piv_t_x);
      f.y = piv_t_y + factor * (f.y - piv_t_y);
    }
    for (int i = 0; i < (int)target.fiducials.size(); ++i)
      yjs_op_fiducial_replace(target.name, i, target.fiducials[i]);
  }

  auto hit = [&](int &out_level, int &out_idx) {
    out_level = -1; out_idx = -1;
    float best = kFidHitPx;
    for (int i = 0; i < (int)ref.fiducials.size(); ++i) {
      auto [wx, wy] = floor_to_canvas(0, ref.fiducials[i].x, ref.fiducials[i].y);
      ImVec2 p = canvas_.world_to_screen(wx, wy);
      float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
      if (d < best) { best = d; out_level = 0; out_idx = i; }
    }
    for (int i = 0; i < (int)target.fiducials.size(); ++i) {
      auto [wx, wy] = floor_to_canvas(tgt, target.fiducials[i].x,
                                      target.fiducials[i].y);
      ImVec2 p = canvas_.world_to_screen(wx, wy);
      float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
      if (d < best) { best = d; out_level = tgt; out_idx = i; }
    }
  };

  static bool s_drag_single = false;
  static bool s_drag_group = false;
  static bool s_moved = false;

  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    int hl = -1, hi = -1;
    hit(hl, hi);
    if (state.align_floors_placing) {
      std::string name = state.align_floors_next_name;
      if (name.empty())
        name = "F" + std::to_string(state.align_floors_next_id);
      auto [wx, wy] = canvas_.screen_to_world(mouse);
      auto [rfx, rfy] = canvas_to_floor(0, wx, wy);
      auto [tfx, tfy] = canvas_to_floor(tgt, wx, wy);
      Fiducial rf, tf;
      rf.name = name; rf.x = rfx; rf.y = rfy;
      tf.name = name; tf.x = tfx; tf.y = tfy;
      ref.fiducials.push_back(rf);
      yjs_op_fiducial_add(ref.name, rf);
      target.fiducials.push_back(tf);
      yjs_op_fiducial_add(target.name, tf);
      state.align_floors_sel_level = tgt;
      state.align_floors_sel_idx = (int)target.fiducials.size() - 1;
      state.align_floors_next_id += 1;
      state.align_floors_next_name =
          "F" + std::to_string(state.align_floors_next_id);
      state.align_floors_placing = false;
      s_drag_single = false;
      s_drag_group = false;
    } else if (hl >= 0) {
      state.align_floors_sel_level = hl;
      state.align_floors_sel_idx = hi;
      s_drag_single = true;
      s_moved = false;
    } else {
      s_drag_group = true;
      s_moved = false;
    }
  }

  if (s_drag_single && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (state.align_floors_sel_level < 0) {
      s_drag_single = false;
    } else if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      int lvl = state.align_floors_sel_level;
      int idx = state.align_floors_sel_idx;
      auto [wx, wy] = canvas_.screen_to_world(mouse);
      auto [fx, fy] = canvas_to_floor(lvl, wx, wy);
      if (lvl == 0 && idx >= 0 && idx < (int)ref.fiducials.size()) {
        ref.fiducials[idx].x = fx;
        ref.fiducials[idx].y = fy;
        s_moved = true;
      } else if (lvl == tgt && idx >= 0 &&
                 idx < (int)target.fiducials.size() && xf.scale > 1e-12) {
        target.fiducials[idx].x = fx;
        target.fiducials[idx].y = fy;
        s_moved = true;
      }
    }
  }

  if (s_drag_group && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !target.fiducials.empty() && xf.scale > 1e-12) {
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      // Compute target-fp delta by diffing canvas_to_floor at two cursor pos.
      ImVec2 prev(mouse.x - io.MouseDelta.x, mouse.y - io.MouseDelta.y);
      auto [w_new_x, w_new_y] = canvas_.screen_to_world(mouse);
      auto [w_old_x, w_old_y] = canvas_.screen_to_world(prev);
      auto [f_new_x, f_new_y] = canvas_to_floor(tgt, w_new_x, w_new_y);
      auto [f_old_x, f_old_y] = canvas_to_floor(tgt, w_old_x, w_old_y);
      double dix = f_new_x - f_old_x;
      double diy = f_new_y - f_old_y;
      for (Fiducial &f : target.fiducials) { f.x -= dix; f.y -= diy; }
      s_moved = true;
    }
  }

  if ((s_drag_single || s_drag_group) &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (s_drag_single && s_moved && state.align_floors_sel_level >= 0) {
      int lvl = state.align_floors_sel_level;
      int idx = state.align_floors_sel_idx;
      if (lvl == 0 && idx >= 0 && idx < (int)ref.fiducials.size())
        yjs_op_fiducial_replace(ref.name, idx, ref.fiducials[idx]);
      else if (lvl == tgt && idx >= 0 && idx < (int)target.fiducials.size())
        yjs_op_fiducial_replace(target.name, idx, target.fiducials[idx]);
    } else if (s_drag_group && s_moved) {
      for (int i = 0; i < (int)target.fiducials.size(); ++i)
        yjs_op_fiducial_replace(target.name, i, target.fiducials[i]);
    }
    s_drag_single = false;
    s_drag_group = false;
    s_moved = false;
  }

  if (state.align_floors_sel_level >= 0 && !io.WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Delete)) {
    int lvl = state.align_floors_sel_level;
    int idx = state.align_floors_sel_idx;
    Level *L = (lvl == 0) ? &ref : (lvl == tgt) ? &target : nullptr;
    if (L && idx >= 0 && idx < (int)L->fiducials.size()) {
      yjs_op_fiducial_delete(L->name, idx);
      L->fiducials.erase(L->fiducials.begin() + idx);
    }
    state.align_floors_sel_level = -1;
    state.align_floors_sel_idx = -1;
  }
}

void EditorView::draw_version_strip(EditorState &state) {
  const bool on_snapshot = !state.snapshot_dir.empty();
  if (on_snapshot)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(60, 40, 30, 255));
  ImGui::BeginChild("##version_strip", ImVec2(0, 0), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::AlignTextToFramePadding();
  if (on_snapshot)
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                       "Read-only snapshot:");
  else
    ImGui::TextUnformatted("Version:");
  ImGui::SameLine();

  std::string preview = on_snapshot ? state.snapshot_dir : std::string("latest");
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo("##version_combo", preview.c_str())) {
    if (ImGui::Selectable("latest", !on_snapshot))
      state.snapshot_request_unload = "1";
    for (const auto &s : state.snapshots) {
      char label[128];
      std::time_t t = (std::time_t)s.created_at;
      std::tm tm_utc{};
#if defined(_WIN32)
      gmtime_s(&tm_utc, &t);
#else
      gmtime_r(&t, &tm_utc);
#endif
      std::snprintf(label, sizeof(label),
                    "%s  %04d-%02d-%02d %02d:%02d", s.sha.c_str(),
                    tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                    tm_utc.tm_hour, tm_utc.tm_min);
      bool sel = (s.dir == state.snapshot_dir);
      if (ImGui::Selectable(label, sel))
        state.snapshot_request_load = s.dir;
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh##snap"))
    state.snapshot_request_refresh = true;
  ImGui::SameLine();
  if (on_snapshot) {
    ImGui::BeginDisabled();
    ImGui::Button("Snapshot");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 80, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 100, 70, 255));
    if (ImGui::Button("Restore to latest"))
      ImGui::OpenPopup("##restore_confirm");
    ImGui::PopStyleColor(2);
  } else {
    if (ImGui::Button("Snapshot"))
      state.snapshot_request_create = true;
  }

  if (ImGui::BeginPopupModal("##restore_confirm", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Overwrite latest with snapshot %s?",
                state.snapshot_dir.c_str());
    ImGui::TextDisabled("Connected users will see the restored state.");
    ImGui::Separator();
    if (ImGui::Button("Restore", ImVec2(120, 0))) {
      state.snapshot_request_restore = state.snapshot_dir;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (!state.snapshot_status.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.snapshot_status.c_str());
  }
  ImGui::EndChild();
  if (on_snapshot)
    ImGui::PopStyleColor();
}

void set_yjs_readonly(bool v) { g_readonly = v; }
bool yjs_readonly() { return g_readonly; }

} // namespace imrmf::map_editor
