// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#define IMGUI_DEFINE_MATH_OPERATORS
#include "view/editor_view.hpp"

#include "imgui/imgui.h"
#include "imgui/misc/cpp/imgui_stdlib.h"

#include "ui/icons.hpp"
#include "ui/widgets.hpp"

#include "model/asset_paths.hpp"
#include "model/yaml_io.hpp"
#include "view/canvas_controls.hpp"

#ifndef __EMSCRIPTEN__
#include "client_rust/client.h"
#endif

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

// optional: numeric that may be absent. Absent = unset, not 0. See
// param_optional().
struct VertexParamSpec {
  const char *key;
  ParamType type;
  bool optional = false;
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
    {"merge_radius", ParamType::DOUBLE, true},
}};

struct LaneParamSpec {
  const char *key;
  ParamType type;
  bool optional = false;
};
const std::array<LaneParamSpec, 7> kLaneParams = {{
    {"bidirectional", ParamType::BOOL},
    {"orientation", ParamType::STRING},
    {"graph_idx", ParamType::INT, true},
    {"speed_limit", ParamType::DOUBLE, true},
    {"mutex", ParamType::STRING},
    {"demo_mock_floor_name", ParamType::STRING},
    {"demo_mock_lift_name", ParamType::STRING},
}};

// Is a param optional (absent == unset)? Both editors consult this, so no call
// site can write 0 for an unset field.
inline bool param_optional(const char *key) {
  for (const auto &s : kVertexParams)
    if (std::strcmp(s.key, key) == 0)
      return s.optional;
  for (const auto &s : kLaneParams)
    if (std::strcmp(s.key, key) == 0)
      return s.optional;
  return false;
}

struct ParamSpec {
  const char *key;
  ParamType type;
};
const std::array<ParamSpec, 5> kWallParams = {{
    {"texture_name", ParamType::STRING},
    {"texture_height", ParamType::DOUBLE},
    {"texture_width", ParamType::DOUBLE},
    {"texture_scale", ParamType::DOUBLE},
    {"alpha", ParamType::DOUBLE},
}};
// type uses a dedicated combo, the rest are plain fields
const std::array<ParamSpec, 3> kDoorParams = {{
    {"name", ParamType::STRING},
    {"motion_degrees", ParamType::DOUBLE},
    {"motion_direction", ParamType::INT},
}};
const std::array<ParamSpec, 3> kFloorParams = {{
    {"texture_name", ParamType::STRING},
    {"texture_rotation", ParamType::DOUBLE},
    {"texture_scale", ParamType::DOUBLE},
}};

void ensure_params(
    std::map<std::string, ParamValue> &p,
    std::initializer_list<std::pair<const char *, ParamValue>> defs) {
  for (const auto &[k, v] : defs)
    if (p.find(k) == p.end())
      p[k] = v;
}
void init_default_wall_params(Wall &w) {
  ensure_params(w.params, {
                              {"texture_name", ParamValue::make_string("")},
                              {"texture_height", ParamValue::make_double(2.5)},
                              {"texture_width", ParamValue::make_double(1.0)},
                              {"texture_scale", ParamValue::make_double(1.0)},
                              {"alpha", ParamValue::make_double(1.0)},
                          });
}
void init_default_door_params(Door &d) {
  ensure_params(d.params, {
                              {"name", ParamValue::make_string("")},
                              {"type", ParamValue::make_string("hinged")},
                              {"motion_axis", ParamValue::make_string("start")},
                              {"motion_degrees", ParamValue::make_double(90.0)},
                              {"motion_direction", ParamValue::make_int(1)},
                          });
}
void init_default_measurement_params(Measurement &m) {
  ensure_params(m.params, {{"distance", ParamValue::make_double(1.0)}});
}
void init_default_floor_params(Floor &f) {
  ensure_params(f.params,
                {
                    {"texture_name", ParamValue::make_string("")},
                    {"texture_rotation", ParamValue::make_double(0.0)},
                    {"texture_scale", ParamValue::make_double(1.0)},
                });
}

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

// Snap a world point to H/V/45 off an anchor vertex while shift is held.
// anchor_idx < 0 or shift off returns the point unchanged.
std::pair<double, double> snap_to_anchor(const Level &level, int anchor_idx,
                                         double wx, double wy, bool shift) {
  if (!shift || anchor_idx < 0 || anchor_idx >= (int)level.vertices.size())
    return {wx, wy};
  const Vertex &a = level.vertices[anchor_idx];
  auto [dx, dy] = snap_axis_or_diagonal(wx - a.x, wy - a.y);
  return {a.x + dx, a.y + dy};
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
void mevjs_wall_add(const char *level, const char *yaml);
void mevjs_wall_replace(const char *level, int idx, const char *yaml);
void mevjs_wall_delete(const char *level, int idx);
void mevjs_door_add(const char *level, const char *yaml);
void mevjs_door_replace(const char *level, int idx, const char *yaml);
void mevjs_door_delete(const char *level, int idx);
void mevjs_measurement_add(const char *level, const char *yaml);
void mevjs_measurement_replace(const char *level, int idx, const char *yaml);
void mevjs_measurement_delete(const char *level, int idx);
void mevjs_floor_add(const char *level, const char *yaml);
void mevjs_floor_replace(const char *level, int idx, const char *yaml);
void mevjs_floor_delete(const char *level, int idx);
void mevjs_layer_set(const char *level, const char *layer_name,
                     const char *yaml);
void mevjs_layer_delete(const char *level, const char *layer_name);
void mevjs_layer_reorder(const char *level, const char *names_json);
void mevjs_fiducial_add(const char *level, const char *yaml);
void mevjs_fiducial_replace(const char *level, int idx, const char *yaml);
void mevjs_fiducial_delete(const char *level, int idx);
void mevjs_set_reference_level(const char *name);
void mevjs_drawing_set(const char *level, const char *filename);
void mev_assets_list(const char *id, const char *subdir);
const char *mev_assets_code();
const char *mev_assets_payload();
void mev_asset_upload(const char *id, const char *dir);
const char *mev_asset_up_code();
const char *mev_asset_up_name();
}
#else
static inline void mev_assets_list(const char *, const char *) {}
static inline const char *mev_assets_code() { return "idle"; }
static inline const char *mev_assets_payload() { return ""; }
static inline void mev_asset_upload(const char *, const char *) {}
static inline const char *mev_asset_up_code() { return "idle"; }
static inline const char *mev_asset_up_name() { return ""; }
#endif

#ifdef __EMSCRIPTEN__
inline void note_local_edit() {} // the browser sends an op per edit
#else
// The desktop has no per-op bridge. It pushes the whole document when the
// editor reports itself dirty, and without this flag no edit ever reaches the
// CRDT, so nothing syncs and undo has nothing to track.
bool g_local_edit = false;
inline void note_local_edit() { g_local_edit = true; }
#endif

void yjs_op_vertex_add(const std::string &level, const Vertex &v) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  std::string y = serialize_vertex(v);
  mevjs_vertex_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)v;
#endif
}
void yjs_op_vertex_replace(const std::string &level, int idx, const Vertex &v) {
  if (g_readonly)
    return;
  note_local_edit();
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
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_vertex_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_lane_add(const std::string &level, const Lane &l) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  std::string y = serialize_lane(l);
  mevjs_lane_add(level.c_str(), y.c_str());
#else
  (void)level;
  (void)l;
#endif
}
void yjs_op_lane_replace(const std::string &level, int idx, const Lane &l) {
  if (g_readonly)
    return;
  note_local_edit();
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
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_lane_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}

#ifdef __EMSCRIPTEN__
#define IMRMF_BRIDGE(call) call;
#else
#define IMRMF_BRIDGE(call)
#endif
// generates the yjs_op_<name>_{add,replace,delete} wrappers
#define IMRMF_GEOM_OPS(Type, name)                                             \
  void yjs_op_##name##_add(const std::string &lvl, const Type &e) {            \
    if (g_readonly)                                                            \
      return;                                                                  \
    (void)lvl;                                                                 \
    (void)e;                                                                   \
    IMRMF_BRIDGE(mevjs_##name##_add(lvl.c_str(), serialize_##name(e).c_str())) \
  }                                                                            \
  void yjs_op_##name##_replace(const std::string &lvl, int idx,                \
                               const Type &e) {                                \
    if (g_readonly)                                                            \
      return;                                                                  \
    (void)lvl;                                                                 \
    (void)idx;                                                                 \
    (void)e;                                                                   \
    IMRMF_BRIDGE(                                                              \
        mevjs_##name##_replace(lvl.c_str(), idx, serialize_##name(e).c_str())) \
  }                                                                            \
  void yjs_op_##name##_delete(const std::string &lvl, int idx) {               \
    if (g_readonly)                                                            \
      return;                                                                  \
    (void)lvl;                                                                 \
    (void)idx;                                                                 \
    IMRMF_BRIDGE(mevjs_##name##_delete(lvl.c_str(), idx))                      \
  }
IMRMF_GEOM_OPS(Wall, wall)
IMRMF_GEOM_OPS(Door, door)
IMRMF_GEOM_OPS(Measurement, measurement)
IMRMF_GEOM_OPS(Floor, floor)
#undef IMRMF_GEOM_OPS
#undef IMRMF_BRIDGE
void yjs_op_layer_set(const std::string &level, const Layer &L) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  std::string y = serialize_layer(L);
  mevjs_layer_set(level.c_str(), L.name.c_str(), y.c_str());
#else
  (void)level;
  (void)L;
#endif
}
void yjs_op_layer_delete(const std::string &level, const std::string &name) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_layer_delete(level.c_str(), name.c_str());
#else
  (void)level;
  (void)name;
#endif
}
void yjs_op_fiducial_add(const std::string &level, const Fiducial &f) {
  if (g_readonly)
    return;
  note_local_edit();
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
  if (g_readonly)
    return;
  note_local_edit();
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
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_fiducial_delete(level.c_str(), idx);
#else
  (void)level;
  (void)idx;
#endif
}
void yjs_op_set_reference_level(const std::string &name) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_set_reference_level(name.c_str());
#else
  (void)name;
#endif
}

void yjs_op_drawing_set(const std::string &level, const std::string &filename) {
  if (g_readonly)
    return;
  note_local_edit();
#ifdef __EMSCRIPTEN__
  mevjs_drawing_set(level.c_str(), filename.c_str());
#else
  (void)level;
  (void)filename;
#endif
}

void yjs_op_layer_reorder(const std::string &level,
                          const std::vector<std::string> &names) {
  if (g_readonly)
    return;
  note_local_edit();
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
  // graph_idx/speed_limit stay optional (absent = RMF default), no 0 per lane.
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

std::string get_mutex(const std::map<std::string, ParamValue> &p) {
  auto it = p.find("mutex");
  if (it != p.end() && it->second.type == ParamType::STRING)
    return it->second.s;
  return "";
}

struct MutexGroupInfo {
  std::string name;
  int lanes = 0;
  int vertices = 0;
};

std::vector<MutexGroupInfo> gather_mutex_groups(const Level &level) {
  std::map<std::string, MutexGroupInfo> m;
  for (const auto &l : level.lanes) {
    std::string s = get_mutex(l.params);
    if (!s.empty())
      m[s].lanes++;
  }
  for (const auto &v : level.vertices) {
    std::string s = get_mutex(v.params);
    if (!s.empty())
      m[s].vertices++;
  }
  std::vector<MutexGroupInfo> out;
  out.reserve(m.size());
  for (auto &[k, info] : m) {
    info.name = k;
    out.push_back(info);
  }
  return out;
}

void draw_param_editor(std::map<std::string, ParamValue> &params,
                       const char *key, ParamType type, bool &dirty,
                       bool &commit) {
  // Avoid params[key] seeding a default on render and persist only on edit.
  auto it = params.find(key);
  ParamValue pv = (it != params.end()) ? it->second : ParamValue{};
  if (pv.type != type) {
    pv.type = type;
    pv.s.clear();
    pv.i = 0;
    pv.d = 0.0;
    pv.b = false;
  }
  bool edited = false;
  ImGui::PushID(key);
  switch (type) {
  case ParamType::BOOL: {
    bool v = pv.b;
    if (ImGui::Checkbox(key, &v)) {
      pv.b = v;
      dirty = true;
      edited = true;
      commit = true;
    }
    break;
  }
  case ParamType::STRING: {
    std::string v = pv.s;
    if (ImGui::InputText(key, &v)) {
      pv.s = std::move(v);
      dirty = true;
      edited = true;
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
      edited = true;
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
      edited = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  }
  ImGui::PopID();
  if (edited)
    params[key] = pv;
}

// Form-row variants: label left, control right. Call inside a BeginFormTable.
static void draw_param_row(std::map<std::string, ParamValue> &params,
                           const char *key, ParamType type, bool &dirty,
                           bool &commit) {
  ImGui::PushID(key);
  ImGuiWidgets::FormRow(key);

  if (param_optional(key)) {
    auto it = params.find(key);
    const bool present = (it != params.end() && it->second.type == type);
    if (!present) {
      if (ImGui::Button("Set")) {
        params[key] = (type == ParamType::INT) ? ParamValue::make_int(0)
                                               : ParamValue::make_double(0.0);
        dirty = commit = true;
      }
    } else {
      const float clear_w = ImGui::GetFrameHeight();
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth() - clear_w -
                              ImGui::GetStyle().ItemSpacing.x);
      if (type == ParamType::INT) {
        int v = it->second.i;
        if (ImGui::InputInt("##v", &v)) {
          params[key] = ParamValue::make_int(v);
          dirty = true;
        }
      } else {
        float v = (float)it->second.d;
        if (ImGui::InputFloat("##v", &v, 0.0f, 0.0f, "%.4f")) {
          params[key] = ParamValue::make_double((double)v);
          dirty = true;
        }
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        commit = true;
      ImGui::SameLine();
      if (ImGui::Button(ICON_MDI_CLOSE, ImVec2(clear_w, 0.0f))) {
        params.erase(key);
        dirty = commit = true;
      }
    }
    ImGui::PopID();
    return;
  }

  auto it = params.find(key);
  ParamValue pv = (it != params.end()) ? it->second : ParamValue{};
  if (pv.type != type) {
    pv.type = type;
    pv.s.clear();
    pv.i = 0;
    pv.d = 0.0;
    pv.b = false;
  }
  bool edited = false;
  switch (type) {
  case ParamType::BOOL: {
    bool v = pv.b;
    if (ImGui::Checkbox("##v", &v)) {
      pv.b = v;
      dirty = edited = commit = true;
    }
    break;
  }
  case ParamType::STRING: {
    std::string v = pv.s;
    ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
    if (ImGui::InputText("##v", &v)) {
      pv.s = std::move(v);
      dirty = edited = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  case ParamType::INT: {
    int v = pv.i;
    ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
    if (ImGui::InputInt("##v", &v)) {
      pv.i = v;
      dirty = edited = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  case ParamType::DOUBLE: {
    float fv = (float)pv.d;
    ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
    if (ImGui::InputFloat("##v", &fv, 0.1f, 1.0f, "%.2f")) {
      pv.d = (double)fv;
      dirty = edited = true;
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
      commit = true;
    break;
  }
  }
  ImGui::PopID();
  if (edited)
    params[key] = pv;
}

static void draw_orientation_row(std::map<std::string, ParamValue> &params,
                                 bool &dirty, bool &commit) {
  auto &pv = params["orientation"];
  if (pv.type != ParamType::STRING) {
    pv.type = ParamType::STRING;
    pv.s.clear();
  }
  const char *items[] = {"(none)", "forward", "backward"};
  int current = (pv.s == "forward") ? 1 : (pv.s == "backward") ? 2 : 0;
  ImGui::PushID("orientation");
  ImGuiWidgets::FormRow("orientation");
  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  if (ImGui::Combo("##v", &current, items, 3)) {
    pv.s = (current == 1) ? "forward" : (current == 2) ? "backward" : "";
    dirty = commit = true;
  }
  ImGui::PopID();
}

static void draw_mutex_row(std::map<std::string, ParamValue> &params,
                           const std::vector<MutexGroupInfo> &groups,
                           EditorState &state, bool &dirty, bool &commit) {
  std::string cur = get_mutex(params);
  const ImGuiStyle &st = ImGui::GetStyle();
  ImGui::PushID("mutex");
  ImGuiWidgets::FormRow("mutex");
  if (!state.mutex_adding) {
    const float plus_w = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth() - plus_w -
                            st.ItemSpacing.x);
    if (ImGui::BeginCombo("##v", cur.empty() ? "(none)" : cur.c_str())) {
      if (ImGui::Selectable("(none)", cur.empty())) {
        params["mutex"] = ParamValue::make_string("");
        dirty = commit = true;
      }
      for (const auto &g : groups) {
        if (ImGui::Selectable(g.name.c_str(), g.name == cur)) {
          params["mutex"] = ParamValue::make_string(g.name);
          dirty = commit = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_MDI_PLUS, ImVec2(plus_w, 0.0f))) {
      state.mutex_adding = true;
      state.mutex_new_buf.clear();
    }
  } else {
    const float set_w = ImGui::CalcTextSize("Set").x + st.FramePadding.x * 2.0f;
    const float cancel_w =
        ImGui::CalcTextSize("Cancel").x + st.FramePadding.x * 2.0f;
    float input_w = ImGuiWidgets::FormControlWidth() - set_w - cancel_w -
                    st.ItemSpacing.x * 2.0f;
    if (input_w < 40.0f)
      input_w = 40.0f;
    ImGui::SetNextItemWidth(input_w);
    ImGui::InputTextWithHint("##newmutex", "new group", &state.mutex_new_buf);
    ImGui::SameLine();
    ImGui::BeginDisabled(state.mutex_new_buf.empty());
    if (ImGui::Button("Set")) {
      params["mutex"] = ParamValue::make_string(state.mutex_new_buf);
      dirty = commit = true;
      state.mutex_adding = false;
      state.mutex_new_buf.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      state.mutex_adding = false;
      state.mutex_new_buf.clear();
    }
  }
  ImGui::PopID();
}

// Bottom-right HUD listing whatever controls are active right now. Drawn to the
// draw list like the coord HUD so it never captures canvas clicks.
void draw_controls_overlay(const canvas::MapCanvas &c,
                           const ControlContext &ctx) {
  auto controls = canvas_controls().active(ctx);
  if (controls.empty())
    return;
  std::string title = control_context_title(ctx);

  const float pad = 8.0f, gap = 12.0f;
  const float lh = ImGui::GetTextLineHeightWithSpacing();
  float key_w = 0.0f, desc_w = 0.0f;
  for (const CanvasControl *ctl : controls) {
    key_w = std::max(key_w, ImGui::CalcTextSize(ctl->chord().c_str()).x);
    desc_w =
        std::max(desc_w, ImGui::CalcTextSize(ctl->description().c_str()).x);
  }
  float inner_w =
      std::max(ImGui::CalcTextSize(title.c_str()).x, key_w + gap + desc_w);
  float w = inner_w + pad * 2.0f;
  float h = pad * 2.0f + lh + 4.0f + lh * (float)controls.size();

  ImVec2 cp = c.canvas_pos(), cs = c.canvas_size();
  ImVec2 tl(cp.x + cs.x - w - 8.0f, cp.y + cs.y - h - 8.0f);
  ImVec2 br(tl.x + w, tl.y + h);
  ImDrawList *dl = c.draw_list();
  dl->AddRectFilled(
      tl, br,
      ImGui::GetColorU32(theme::with_alpha(theme::palette::surface, 0.92f)),
      6.0f);
  dl->AddRect(tl, br, ImGui::GetColorU32(theme::palette::border), 6.0f);

  float x = tl.x + pad, y = tl.y + pad;
  dl->AddText(ImVec2(x, y), ImGui::GetColorU32(theme::palette::blue),
              title.c_str());
  y += lh + 4.0f;
  for (const CanvasControl *ctl : controls) {
    dl->AddText(ImVec2(x, y), ImGui::GetColorU32(theme::palette::accent),
                ctl->chord().c_str());
    dl->AddText(ImVec2(x + key_w + gap, y),
                ImGui::GetColorU32(theme::palette::muted),
                ctl->description().c_str());
    y += lh;
  }
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
EM_JS(void, mevjs_wall_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.wallAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_wall_replace, (const char *level, int idx, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.wallReplace(UTF8ToString(level), idx, UTF8ToString(yaml));
});
EM_JS(void, mevjs_wall_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.wallDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_door_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.doorAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_door_replace, (const char *level, int idx, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.doorReplace(UTF8ToString(level), idx, UTF8ToString(yaml));
});
EM_JS(void, mevjs_door_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.doorDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_measurement_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.measurementAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_measurement_replace,
      (const char *level, int idx, const char *yaml), {
        if (window.imrmf.yjs)
          window.imrmf.yjs.measurementReplace(UTF8ToString(level), idx,
                                              UTF8ToString(yaml));
      });
EM_JS(void, mevjs_measurement_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.measurementDelete(UTF8ToString(level), idx);
});
EM_JS(void, mevjs_floor_add, (const char *level, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.floorAdd(UTF8ToString(level), UTF8ToString(yaml));
});
EM_JS(void, mevjs_floor_replace, (const char *level, int idx, const char *yaml), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.floorReplace(UTF8ToString(level), idx, UTF8ToString(yaml));
});
EM_JS(void, mevjs_floor_delete, (const char *level, int idx), {
  if (window.imrmf.yjs)
    window.imrmf.yjs.floorDelete(UTF8ToString(level), idx);
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
EM_JS(void, mevjs_drawing_set, (const char *level, const char *filename), {
  if (window.imrmf.yjs && window.imrmf.yjs.drawingSet)
    window.imrmf.yjs.drawingSet(UTF8ToString(level), UTF8ToString(filename));
});

// Results land in polled slots (ImGui is immediate-mode).
EM_JS(void, mev_assets_list, (const char *id_c, const char *subdir_c), {
  if (!window.imrmf)
    return;
  window.imrmf._assets = {code : 'busy', payload : null};
  let base = window.location.origin || "";
  while (base.length && base[base.length - 1] === '/')
    base = base.slice(0, -1);
  const id = UTF8ToString(id_c), sub = UTF8ToString(subdir_c);
  const url = base + "/assets/list?id=" + encodeURIComponent(id) +
              (sub ? "&subdir=" + encodeURIComponent(sub) : "");
  fetch(url)
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(d => { window.imrmf._assets = {code : 'ok', payload : d}; })
      .catch(e => {
        window.imrmf._assets = {code : 'err', payload : String(e)};
      });
});
EM_JS(const char *, mev_assets_code, (), {
  return stringToNewUTF8(
      (window.imrmf && window.imrmf._assets && window.imrmf._assets.code) ||
      'idle');
});
EM_JS(const char *, mev_assets_payload, (), {
  if (!window.imrmf || !window.imrmf._assets)
    return stringToNewUTF8("");
  const p = window.imrmf._assets.payload;
  return stringToNewUTF8(
      p == null ? "" : (typeof p === 'string' ? p : JSON.stringify(p)));
});
EM_JS(void, mev_asset_upload, (const char *id_c, const char *dir_c), {
  if (!window.imrmf)
    return;
  const id = UTF8ToString(id_c), dir = UTF8ToString(dir_c);
  let base = window.location.origin || "";
  while (base.length && base[base.length - 1] === '/')
    base = base.slice(0, -1);
  window.imrmf._assets_up = {code : 'idle', name : ""};
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = 'image/*';
  inp.onchange = () => {
    const f = inp.files && inp.files[0];
    if (!f) {
      window.imrmf._assets_up = {code : 'idle', name : ""};
      return;
    }
    window.imrmf._assets_up = {code : 'busy', name : f.name};
    const path = (dir ? dir + '/' : "") + f.name;
    f.arrayBuffer()
        .then(buf => fetch(base + "/layer_asset?id=" + encodeURIComponent(id) +
                               "&path=" + encodeURIComponent(path),
                           {method : 'PUT', body : buf}))
        .then(r => r.ok ? r.text() : r.text().then(t => Promise.reject(t)))
        .then(() => { window.imrmf._assets_up = {code : 'ok', name : f.name}; })
        .catch(e => {
          window.imrmf._assets_up = {code : 'err', name : String(e)};
        });
  };
  inp.click();
});
EM_JS(const char *, mev_asset_up_code, (), {
  return stringToNewUTF8((window.imrmf && window.imrmf._assets_up &&
                          window.imrmf._assets_up.code) ||
                         'idle');
});
EM_JS(const char *, mev_asset_up_name, (), {
  return stringToNewUTF8((window.imrmf && window.imrmf._assets_up &&
                          window.imrmf._assets_up.name) ||
                         "");
});

#endif // __EMSCRIPTEN__

} // namespace

EditorView::EditorView(std::unique_ptr<canvas::TextureProvider> provider,
                       std::string building_id)
    : building_id_(std::move(building_id)),
      texture_provider_(std::move(provider)),
      http_provider_(
          dynamic_cast<canvas::HttpTextureProvider *>(texture_provider_.get())),
      canvas_(building_id_, texture_provider_.get()) {
  // So the provider can fetch on its own, which packing a bundle needs.
  if (http_provider_)
    http_provider_->set_asset_id(building_id_);
}

EditorView::~EditorView() = default;

void EditorView::apply_snapshot_dir(const std::string &dir) {
  // Snapshots are a server concept, a local map has no route to rewrite.
  if (!http_provider_)
    return;
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
    http_provider_->set_url_builder(
        [enc](const std::string &id, const std::string &path) {
          return "/layer_asset?id=" + enc(id) + "&path=" + enc(path);
        });
  } else {
    std::string d = dir;
    http_provider_->set_url_builder(
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

#ifndef __EMSCRIPTEN__
  // Edits from the previous frame's ops, which the app pushes on.
  if (g_local_edit) {
    g_local_edit = false;
    state.dirty = true;
  }
#endif

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
  ImVec2 canvas_region(region.x - right_col_w - 8.0f, region.y);
  if (canvas_region.x < 100.0f)
    canvas_region.x = 100.0f;

  ImGui::BeginChild("canvas_region", canvas_region, false,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  {
    const float version_h = 32.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild(
        "##canvas_box", ImVec2(avail.x, avail.y - version_h), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_canvas(building, state);
    ImGui::EndChild();
    draw_version_strip(state, top_bar);
  }
  ImGui::EndChild();

  ImGui::SameLine();
  ImGui::BeginChild("right_col", ImVec2(right_col_w, region.y), false);
  if (state.align_floors_mode) {
    draw_align_floors_panel(building, state);
  } else {
    draw_building_panel(building, state);
    ImGui::Separator();
    draw_mutex_groups_panel(building, state);
    ImGui::Separator();
    draw_layer_config_panel(building, state);
    draw_add_layer_section(building, state);
    draw_layer_browse_modal(building, state);
    ImGui::Separator();
    draw_attribute_panel(building, state);
  }
  ImGui::EndChild();

  if (!ImGui::GetIO().WantTextInput) {
    // `S` collides with Ctrl+S (Save); require no modifier.
    if (ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().KeyCtrl &&
        !ImGui::GetIO().KeySuper)
      state.mode = Mode::Pan;
    if (ImGui::IsKeyPressed(ImGuiKey_V))
      state.mode = Mode::Vertex;
    if (ImGui::IsKeyPressed(ImGuiKey_L))
      state.mode = Mode::Lane;
    if (ImGui::IsKeyPressed(ImGuiKey_W))
      state.mode = Mode::Wall;
    if (ImGui::IsKeyPressed(ImGuiKey_D))
      state.mode = Mode::Door;
    if (ImGui::IsKeyPressed(ImGuiKey_M))
      state.mode = Mode::Measurement;
    if (ImGui::IsKeyPressed(ImGuiKey_F))
      state.mode = Mode::Floor;
    if (ImGui::IsKeyPressed(ImGuiKey_H))
      state.mode = Mode::Hole;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      state.pending_lane_start = -1;
      state.pending_edge_start = -1;
      state.pending_polygon.clear();
    }
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
      {
        auto del = [&](std::vector<int> &sel, auto &edges, auto del_op) {
          std::sort(sel.begin(), sel.end(), std::greater<int>());
          for (int i : sel) {
            if (i < 0 || i >= (int)edges.size())
              continue;
            del_op(level.name, i);
            edges.erase(edges.begin() + i);
          }
          sel.clear();
        };
        del(state.selected_walls, level.walls,
            [](const std::string &l, int i) { yjs_op_wall_delete(l, i); });
        del(state.selected_doors, level.doors,
            [](const std::string &l, int i) { yjs_op_door_delete(l, i); });
        del(state.selected_measurements, level.measurements,
            [](const std::string &l, int i) {
              yjs_op_measurement_delete(l, i);
            });
        if (state.selected_floor >= 0 &&
            state.selected_floor < (int)level.floors.size()) {
          yjs_op_floor_delete(level.name, state.selected_floor);
          level.floors.erase(level.floors.begin() + state.selected_floor);
          state.selected_floor = -1;
        }
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
  // CRDT-backed undo / redo. A 500ms capture timeout batches contiguous small
  // pushes (a single drag) into one step, in JS and in the desktop client.
  {
    // After an undo the document already holds the wanted state. Pushing our
    // pre-undo copy back would land as a fresh edit and wipe the redo stack.
    auto settle = [&state]() {
#ifndef __EMSCRIPTEN__
      g_local_edit = false;
      state.dirty = false;
#else
      (void)state;
#endif
    };
    // Edits are recorded a beat after they happen, so anything still pending
    // has to land before undo starts rewinding.
    auto flush = [&top_bar]() {
      if (top_bar.on_flush_edits)
        top_bar.on_flush_edits();
    };
    auto do_undo = [&settle, &flush, &top_bar]() {
      flush();
      if (top_bar.on_undo) {
        top_bar.on_undo();
        settle();
        return;
      }
#ifdef __EMSCRIPTEN__
      map_editor_yjs_undo();
#else
      imrmf_client_undo();
#endif
      settle();
    };
    auto do_redo = [&settle, &flush, &top_bar]() {
      flush();
      if (top_bar.on_redo) {
        top_bar.on_redo();
        settle();
        return;
      }
#ifdef __EMSCRIPTEN__
      map_editor_yjs_redo();
#else
      imrmf_client_redo();
#endif
      settle();
    };
    const bool mod = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper;
    if (mod && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Z)) {
      if (ImGui::GetIO().KeyShift)
        do_redo();
      else
        do_undo();
    }
    if (mod && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Y)) {
      do_redo();
    }
  }

  // Idle-commit if the pending edit has been quiet long enough.
  if (state.pending_commit_time > 0.0 &&
      (ImGui::GetTime() - state.pending_commit_time) > kIdleCommitSeconds) {
    flush_all_pending(building.levels[state.level_idx], state);
  }

  if (state.pending_vertex_delete) {
    ImGui::OpenPopup("Confirm delete");
    state.pending_vertex_delete = false;
  }
  if (ImGuiWidgets::BeginModal("Confirm delete", 360.0f)) {
    Level &level = building.levels[state.level_idx];
    int idx = state.pending_vertex_delete_idx;
    auto refs = (idx >= 0 && idx < (int)level.vertices.size())
                    ? lanes_referencing_vertex(level, idx)
                    : std::vector<int>{};
    ImGui::Text("Vertex %d is referenced by %d lane(s).", idx,
                (int)refs.size());
    ImGui::TextDisabled("Delete vertex and dependent lanes?");
    const int a = ImGuiWidgets::ModalActions("Delete", "Cancel");
    if (a == 1) {
      yjs_op_vertex_delete(level.name, idx);
      delete_vertex(level, idx);
      state.selected_vertices.clear();
      state.pending_vertex_delete_idx = -1;
      ImGui::CloseCurrentPopup();
    } else if (a == 2) {
      state.pending_vertex_delete_idx = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGuiWidgets::EndModal();
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
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", shown.c_str());
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", top_bar.details.empty()
                                  ? top_bar.connection_label.c_str()
                                  : top_bar.details.c_str());
    if (ImGui::IsItemClicked())
      ImGui::OpenPopup("Connection##conn_info");
    ImGui::SameLine();
    if (top_bar.on_save_in_place) {
      if (top_bar.dirty)
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::warning);
      if (ImGui::SmallButton(ICON_MDI_CONTENT_SAVE))
        top_bar.on_save_in_place();
      if (top_bar.dirty)
        ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(top_bar.dirty ? "Save to the same file (Ctrl+S) "
                                          "\xE2\x80\x94 unsaved changes"
                                        : "Save to the same file (Ctrl+S)");
      }
      ImGui::SameLine();
    }
  }
  if (ImGuiWidgets::BeginModal("Connection##conn_info", 420.0f)) {
    ImGui::TextWrapped("Where this map is loaded from, and where your edits "
                       "are written back to.");
    ImGuiWidgets::SectionGap();

    if (!top_bar.detail_rows.empty()) {
      if (ImGuiWidgets::BeginFormTable("##conn_detail")) {
        for (const auto &[label, value] : top_bar.detail_rows) {
          ImGuiWidgets::FormRow(label.c_str());
          ImGui::TextWrapped("%s", value.c_str());
        }
        ImGui::EndTable();
      }
    } else {
      ImGui::TextWrapped("%s", top_bar.details.empty()
                                   ? top_bar.connection_label.c_str()
                                   : top_bar.details.c_str());
    }

    // Offered even in locked mode, since getting back out is the point.
    const bool dc = (bool)top_bar.on_disconnect;
    if (dc) {
      ImGuiWidgets::SectionGap();
      // TextDisabled does not wrap, so dim the colour and use TextWrapped.
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped(
          "%s closes this map and returns to the chooser, where you can open a "
          "local file or mount an S3 bucket.",
          top_bar.disconnect_label.c_str());
      ImGui::PopStyleColor();
    }

    const int action = ImGuiWidgets::ModalActions(
        dc ? top_bar.disconnect_label.c_str() : "Dismiss",
        dc ? "Dismiss" : nullptr,
        /*primary_enabled=*/true, /*primary_danger=*/dc);
    if (action == 1) {
      if (dc)
        top_bar.on_disconnect();
      ImGui::CloseCurrentPopup();
    } else if (action == 2) {
      ImGui::CloseCurrentPopup();
    }
    ImGuiWidgets::EndModal();
  }
#ifndef __EMSCRIPTEN__
  if (top_bar.has_server) {
    const bool connected = imrmf_client_is_connected() != 0;
    const bool ok = connected && imrmf_client_is_synced() != 0;
    const ImVec4 col = ok          ? theme::palette::success
                       : connected ? theme::palette::warning
                                   : theme::palette::danger;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGuiWidgets::IconText(ICON_MDI_ACCOUNT_MULTIPLE, "collab");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", ok          ? "connected (synced)"
                              : connected ? "connected, syncing"
                                          : "not connected");
    }
    ImGui::SameLine();
  }
#endif
#ifdef __EMSCRIPTEN__
  if (top_bar.has_server) {
    const char *yjs = map_editor_yjs_status();
    if (yjs && yjs[0]) {
      bool ok = std::strcmp(yjs, "connected") == 0 && map_editor_yjs_synced();
      const ImVec4 col =
          ok ? theme::palette::success
             : (std::strcmp(yjs, "connecting") == 0 ? theme::palette::warning
                                                    : theme::palette::danger);
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      ImGuiWidgets::IconText(ICON_MDI_ACCOUNT_MULTIPLE, "collab");
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        char tip[64];
        std::snprintf(tip, sizeof(tip), "%s%s", yjs, ok ? " (synced)" : "");
        ImGui::SetTooltip("%s", tip);
      }
      ImGui::SameLine();
    }
    if (yjs)
      std::free((void *)yjs);
  }
#endif
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("|");
  ImGui::SameLine();

  bool can_undo = false, can_redo = false;
  if (top_bar.can_undo && top_bar.can_redo) {
    can_undo = top_bar.can_undo();
    can_redo = top_bar.can_redo();
  } else {
#ifdef __EMSCRIPTEN__
    can_undo = map_editor_yjs_can_undo() != 0;
    can_redo = map_editor_yjs_can_redo() != 0;
#else
    can_undo = imrmf_client_can_undo() != 0;
    can_redo = imrmf_client_can_redo() != 0;
#endif
  }
  if (!can_undo)
    ImGui::BeginDisabled();
  if (ImGui::Button(ICON_MDI_UNDO)) {
    if (top_bar.on_flush_edits)
      top_bar.on_flush_edits();
    if (top_bar.on_undo) {
      top_bar.on_undo();
    } else {
#ifdef __EMSCRIPTEN__
      map_editor_yjs_undo();
#else
      imrmf_client_undo();
#endif
    }
#ifndef __EMSCRIPTEN__
    g_local_edit = false;
    state.dirty = false;
#endif
  }
  if (!can_undo)
    ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Undo (Ctrl+Z)");
  ImGui::SameLine();
  if (!can_redo)
    ImGui::BeginDisabled();
  if (ImGui::Button(ICON_MDI_REDO)) {
    if (top_bar.on_flush_edits)
      top_bar.on_flush_edits();
    if (top_bar.on_redo) {
      top_bar.on_redo();
    } else {
#ifdef __EMSCRIPTEN__
      map_editor_yjs_redo();
#else
      imrmf_client_redo();
#endif
    }
#ifndef __EMSCRIPTEN__
    g_local_edit = false;
    state.dirty = false;
#endif
  }
  if (!can_redo)
    ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Redo (Ctrl+Y / Ctrl+Shift+Z)");
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("|");
  ImGui::SameLine();

  auto mode_button = [&](const char *icon, const char *tip, Mode m,
                         bool same_line = true) {
    if (ImGuiWidgets::ToolbarToggle(icon, tip, state.mode == m)) {
      state.mode = m;
      state.pending_lane_start = -1;
      state.pending_edge_start = -1;
      state.pending_polygon.clear();
    }
    if (same_line)
      ImGui::SameLine();
  };
  auto tool_sep = []() { ImGuiWidgets::ToolbarSeparator(); };
  mode_button(ICON_MDI_CURSOR_DEFAULT, "Select [S]", Mode::Pan);

  {
    const int nv = (int)state.selected_vertices.size();
    const int nl = (int)state.selected_lanes.size();
    if (nv > 0 || nl > 0) {
      ImGui::AlignTextToFramePadding();
      if (nv > 0 && nl > 0)
        ImGui::TextColored(theme::palette::info, "%d vertex / %d lane%s", nv,
                           nl, nl == 1 ? "" : "s");
      else if (nv > 0)
        ImGui::TextColored(theme::palette::info, "%d vertex%s", nv,
                           nv == 1 ? "" : "es");
      else
        ImGui::TextColored(theme::palette::info, "%d lane%s", nl,
                           nl == 1 ? "" : "s");
      ImGui::SameLine();

      std::vector<int> implied_lanes;
      for (int v : state.selected_vertices) {
        for (int li : lanes_referencing_vertex(level, v)) {
          if (std::find(state.selected_lanes.begin(),
                        state.selected_lanes.end(),
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
        // Lanes first (descending) so indices stay valid. Vertex delete
        // cascades via Yjs.
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
          if (std::find(all_lanes.begin(), all_lanes.end(), li) ==
              all_lanes.end())
            all_lanes.push_back(li);
        }
        delete_lane_indices(all_lanes);
        state.selected_lanes.clear();
      };

      if (nv > 0 && any_lanes_affected) {
        if (ImGui::Button(ICON_MDI_DELETE))
          ImGui::OpenPopup("delete_combo_popup");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Delete\xE2\x80\xA6");
        if (ImGui::BeginPopup("delete_combo_popup")) {
          if (ImGui::Selectable("Delete vertices and lanes"))
            delete_all_selected();
          if (ImGui::Selectable("Delete lanes only"))
            delete_only_lanes();
          ImGui::EndPopup();
        }
      } else if (nv > 0) {
        if (ImGui::Button(ICON_MDI_DELETE))
          delete_all_selected();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Delete");
      } else {
        if (ImGui::Button(ICON_MDI_DELETE))
          delete_only_lanes();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Delete");
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
        if (ImGui::Button(ICON_MDI_ALIGN_VERTICAL_CENTER))
          ImGui::OpenPopup("align_h_popup");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Align Vertically");
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
        if (ImGui::Button(ICON_MDI_ALIGN_HORIZONTAL_CENTER))
          ImGui::OpenPopup("align_v_popup");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Align Horizontally");
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
      ImGui::SameLine();
    }
  }

  tool_sep();
  mode_button(ICON_MDI_VECTOR_POINT, "Vertex [V]", Mode::Vertex);
  mode_button(ICON_MDI_VECTOR_POLYLINE, "Lane [L]", Mode::Lane);
  tool_sep();
  mode_button(ICON_MDI_WALL, "Wall [W]", Mode::Wall);
  mode_button(ICON_MDI_DOOR, "Door [D]", Mode::Door);
  mode_button(ICON_MDI_TEXTURE_BOX, "Floor [F]", Mode::Floor);
  mode_button(ICON_MDI_VECTOR_POLYGON_VARIANT, "Hole [H]", Mode::Hole);
  tool_sep();

  {
    if (ImGuiWidgets::ToolbarToggle(ICON_MDI_LAYERS_EDIT, "Align floors",
                                    state.align_floors_mode)) {
      state.align_floors_mode = !state.align_floors_mode;
      if (state.align_floors_mode) {
        state.selected_vertices.clear();
        state.selected_lanes.clear();
        state.selected_layer = -1;
        state.align_layer_idx = -1;
        state.pending_lane_start = -1;
        state.align_floors_sel_level = -1;
        state.align_floors_sel_idx = -1;
        if (state.align_floors_target < 0 || state.align_floors_target == 0 ||
            state.align_floors_target >= (int)building.levels.size()) {
          state.align_floors_target = building.levels.size() >= 2 ? 1 : -1;
        }
        int max_id = 0;
        for (const Level &lvl : building.levels) {
          for (const Fiducial &f : lvl.fiducials) {
            if (f.name.size() > 1 && f.name[0] == 'F') {
              try {
                max_id = std::max(max_id, std::stoi(f.name.substr(1)));
              } catch (...) {
              }
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
    ImGui::SameLine();
  }

  // No trailing SameLine so the divider below spans full width.
  mode_button(ICON_MDI_RULER, "Measure [M]", Mode::Measurement, false);
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
  opts.draw_floors = state.show_floors;
  opts.draw_walls = state.show_walls;
  opts.draw_doors = state.show_doors;
  opts.draw_measurements = state.show_measurements;
  opts.highlight_mutex = state.active_mutex_group;
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
    ImDrawList *gdl = c.draw_list();
    auto vok = [&](int i) { return i >= 0 && i < (int)level.vertices.size(); };
    auto edge_hl = [&](const std::vector<int> &sel, auto &edges, ImU32 col) {
      for (int i : sel) {
        if (i < 0 || i >= (int)edges.size())
          continue;
        const auto &e = edges[i];
        if (!vok(e.start_idx) || !vok(e.end_idx))
          continue;
        ImVec2 a = c.world_to_screen(level.vertices[e.start_idx].x,
                                     level.vertices[e.start_idx].y);
        ImVec2 b = c.world_to_screen(level.vertices[e.end_idx].x,
                                     level.vertices[e.end_idx].y);
        gdl->AddLine(a, b, col, 7.0f);
      }
    };
    edge_hl(state.selected_walls, level.walls, IM_COL32(120, 180, 255, 200));
    edge_hl(state.selected_doors, level.doors, IM_COL32(255, 190, 90, 200));
    edge_hl(state.selected_measurements, level.measurements,
            IM_COL32(255, 240, 150, 200));
    if (state.selected_floor >= 0 &&
        state.selected_floor < (int)level.floors.size()) {
      const Floor &f = level.floors[state.selected_floor];
      for (size_t k = 0; k < f.vertices.size(); ++k) {
        int va = f.vertices[k], vb = f.vertices[(k + 1) % f.vertices.size()];
        if (!vok(va) || !vok(vb))
          continue;
        gdl->AddLine(
            c.world_to_screen(level.vertices[va].x, level.vertices[va].y),
            c.world_to_screen(level.vertices[vb].x, level.vertices[vb].y),
            IM_COL32(255, 255, 255, 220), 2.5f);
      }
    }

    // In-progress edge / polygon previews. The ghost end follows the same
    // shift snap that placement uses.
    ImVec2 cursor = ImGui::GetIO().MousePos;
    bool shift = ImGui::GetIO().KeyShift;
    auto snapped_cursor = [&](int anchor) {
      auto [cwx, cwy] = c.screen_to_world(cursor);
      auto [sx, sy] = snap_to_anchor(level, anchor, cwx, cwy, shift);
      return c.world_to_screen(sx, sy);
    };
    if (mode_is_edge(state.mode) && state.mode != Mode::Lane &&
        vok(state.pending_edge_start)) {
      ImVec2 a = c.world_to_screen(level.vertices[state.pending_edge_start].x,
                                   level.vertices[state.pending_edge_start].y);
      gdl->AddLine(a, snapped_cursor(state.pending_edge_start),
                   IM_COL32(180, 200, 255, 150), 2.0f);
      gdl->AddCircle(a, 9.5f, IM_COL32(80, 255, 120, 255), 0, 2.0f);
    }
    if (mode_is_polygon(state.mode) && !state.pending_polygon.empty()) {
      for (size_t k = 0; k + 1 < state.pending_polygon.size(); ++k) {
        int va = state.pending_polygon[k], vb = state.pending_polygon[k + 1];
        if (vok(va) && vok(vb))
          gdl->AddLine(
              c.world_to_screen(level.vertices[va].x, level.vertices[va].y),
              c.world_to_screen(level.vertices[vb].x, level.vertices[vb].y),
              IM_COL32(150, 220, 150, 200), 2.0f);
      }
      int last = state.pending_polygon.back();
      if (vok(last))
        gdl->AddLine(
            c.world_to_screen(level.vertices[last].x, level.vertices[last].y),
            snapped_cursor(last), IM_COL32(150, 220, 150, 120), 2.0f);
      int first = state.pending_polygon.front();
      if (vok(first))
        gdl->AddCircle(
            c.world_to_screen(level.vertices[first].x, level.vertices[first].y),
            9.5f, IM_COL32(80, 255, 120, 255), 0, 2.0f);
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
    state.selected_walls.clear();
    state.selected_doors.clear();
    state.selected_measurements.clear();
    state.selected_floor = -1;
    state.pending_lane_start = -1;
    state.pending_edge_start = -1;
    state.pending_polygon.clear();
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
  canvas::OverlayViewSettings view;
  view.show_fiducials = &state.show_fiducials;
  view.show_floors = &state.show_floors;
  view.show_walls = &state.show_walls;
  view.show_doors = &state.show_doors;
  view.show_measurements = &state.show_measurements;
  canvas::draw_layers_overlay(building, state.level_idx,
                              state.floorplan_session, state.layer_session,
                              layers_overlay_state_, canvas_, lcb, view);
  state.selected_layer = layers_overlay_state_.selected_layer;
  if (!state.show_fiducials)
    state.selected_fiducial_idx = -1;

  ControlContext cctx;
  cctx.mode = state.mode;
  cctx.layer_align = state.align_layer_idx >= 0 &&
                     state.align_layer_idx < (int)level.layers.size();
  draw_controls_overlay(canvas_, cctx);

  ImGui::SetCursorScreenPos(canvas_.canvas_pos());
  ImGui::InvisibleButton("##canvas", canvas_.canvas_size(),
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);
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
        if (d < best_d) {
          best_d = d;
          best = i;
        }
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
  auto hit_edge = [&](ImVec2 m, auto &edges) -> int {
    int best = -1;
    float best_d = kLaneHitPx;
    for (int i = 0; i < (int)edges.size(); ++i) {
      const auto &e = edges[i];
      if (e.start_idx < 0 || e.start_idx >= (int)level.vertices.size() ||
          e.end_idx < 0 || e.end_idx >= (int)level.vertices.size())
        continue;
      ImVec2 a = world_to_screen(level.vertices[e.start_idx].x,
                                 level.vertices[e.start_idx].y);
      ImVec2 b = world_to_screen(level.vertices[e.end_idx].x,
                                 level.vertices[e.end_idx].y);
      float d = dist_point_segment(m.x, m.y, a.x, a.y, b.x, b.y);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    return best;
  };
  auto hit_floor = [&](ImVec2 m) -> int {
    for (int i = (int)level.floors.size() - 1; i >= 0; --i) {
      const Floor &f = level.floors[i];
      if (f.vertices.size() < 3)
        continue;
      bool inside = false;
      for (size_t a = 0, b = f.vertices.size() - 1; a < f.vertices.size();
           b = a++) {
        int ia = f.vertices[a], ib = f.vertices[b];
        if (ia < 0 || ia >= (int)level.vertices.size() || ib < 0 ||
            ib >= (int)level.vertices.size())
          continue;
        ImVec2 pa = world_to_screen(level.vertices[ia].x, level.vertices[ia].y);
        ImVec2 pb = world_to_screen(level.vertices[ib].x, level.vertices[ib].y);
        if (((pa.y > m.y) != (pb.y > m.y)) &&
            (m.x < (pb.x - pa.x) * (m.y - pa.y) / (pb.y - pa.y) + pa.x))
          inside = !inside;
      }
      if (inside)
        return i;
    }
    return -1;
  };

  auto commit_polygon = [&]() {
    if (state.pending_polygon.size() < 3) {
      state.pending_polygon.clear();
      return;
    }
    if (state.mode == Mode::Floor) {
      Floor f;
      f.vertices = state.pending_polygon;
      init_default_floor_params(f);
      yjs_op_floor_add(level.name, f);
      level.floors.push_back(f);
      state.selected_floor = (int)level.floors.size() - 1;
    } else if (state.selected_floor >= 0 &&
               state.selected_floor < (int)level.floors.size()) {
      Floor &f = level.floors[state.selected_floor];
      f.holes.push_back(state.pending_polygon);
      yjs_op_floor_replace(level.name, state.selected_floor, f);
    }
    state.pending_polygon.clear();
  };

  static bool s_dragging = false;
  static bool s_drag_moved_vertices = false;
  static ImVec2 s_mouse_down_screen{0, 0};
  static double s_drag_start_world_x = 0.0, s_drag_start_world_y = 0.0;
  static std::vector<std::pair<double, double>> s_drag_origins;

  constexpr float kClickThresholdPx = 4.0f;

  if (hovered && state.mode != Mode::Pan) {
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

  if (mode_is_polygon(state.mode) && !ImGui::GetIO().WantTextInput &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
    commit_polygon();
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
    int widx = (vidx < 0 && lidx < 0) ? hit_edge(mouse, level.walls) : -1;
    int didx =
        (vidx < 0 && lidx < 0 && widx < 0) ? hit_edge(mouse, level.doors) : -1;
    int midx = (vidx < 0 && lidx < 0 && widx < 0 && didx < 0)
                   ? hit_edge(mouse, level.measurements)
                   : -1;

    const bool shift = ImGui::GetIO().KeyShift;
    auto toggle_lane = [&](int li) {
      auto it = std::find(state.selected_lanes.begin(),
                          state.selected_lanes.end(), li);
      if (it == state.selected_lanes.end())
        state.selected_lanes.push_back(li);
      else
        state.selected_lanes.erase(it);
    };
    auto clear_geom_sel = [&]() {
      state.selected_walls.clear();
      state.selected_doors.clear();
      state.selected_measurements.clear();
      state.selected_floor = -1;
    };
    switch (state.mode) {
    case Mode::Pan: {
      if (!shift)
        clear_geom_sel();
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
      } else if (widx >= 0) {
        state.selected_vertices.clear();
        state.selected_lanes.clear();
        state.selected_walls = {widx};
      } else if (didx >= 0) {
        state.selected_vertices.clear();
        state.selected_lanes.clear();
        state.selected_doors = {didx};
      } else if (midx >= 0) {
        state.selected_vertices.clear();
        state.selected_lanes.clear();
        state.selected_measurements = {midx};
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
    case Mode::Wall:
    case Mode::Door:
    case Mode::Measurement: {
      int vi = vidx;
      if (vi < 0) {
        Vertex nv;
        auto [nx, ny] =
            snap_to_anchor(level, state.pending_edge_start,
                           s_drag_start_world_x, s_drag_start_world_y, shift);
        nv.x = nx;
        nv.y = ny;
        yjs_op_vertex_add(level.name, nv);
        level.vertices.push_back(nv);
        vi = (int)level.vertices.size() - 1;
      }
      if (state.pending_edge_start < 0) {
        state.pending_edge_start = vi;
      } else if (state.pending_edge_start != vi) {
        int s = state.pending_edge_start;
        if (state.mode == Mode::Wall) {
          Wall w;
          w.start_idx = s;
          w.end_idx = vi;
          init_default_wall_params(w);
          yjs_op_wall_add(level.name, w);
          level.walls.push_back(w);
        } else if (state.mode == Mode::Door) {
          Door d;
          d.start_idx = s;
          d.end_idx = vi;
          init_default_door_params(d);
          yjs_op_door_add(level.name, d);
          level.doors.push_back(d);
        } else {
          Measurement m;
          m.start_idx = s;
          m.end_idx = vi;
          init_default_measurement_params(m);
          yjs_op_measurement_add(level.name, m);
          level.measurements.push_back(m);
        }
        state.pending_edge_start = vi;
      }
      break;
    }
    case Mode::Floor:
    case Mode::Hole: {
      if ((int)state.pending_polygon.size() >= 3 && vidx >= 0 &&
          vidx == state.pending_polygon.front()) {
        commit_polygon();
        break;
      }
      int vi = vidx;
      if (vi < 0) {
        int anchor =
            state.pending_polygon.empty() ? -1 : state.pending_polygon.back();
        Vertex nv;
        auto [nx, ny] = snap_to_anchor(level, anchor, s_drag_start_world_x,
                                       s_drag_start_world_y, shift);
        nv.x = nx;
        nv.y = ny;
        yjs_op_vertex_add(level.name, nv);
        level.vertices.push_back(nv);
        vi = (int)level.vertices.size() - 1;
      }
      if (state.pending_polygon.empty() || state.pending_polygon.back() != vi)
        state.pending_polygon.push_back(vi);
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
      } else if (state.mode == Mode::Pan) {
        // fall back to the floor only when the click selected nothing else
        if (state.selected_vertices.empty() && state.selected_lanes.empty()) {
          int fi = hit_floor(mouse);
          if (fi >= 0)
            state.selected_floor = fi;
        }
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
          auto [vx, vy] = snap_to_anchor(
              level, state.pending_lane_start, s_drag_start_world_x,
              s_drag_start_world_y, ImGui::GetIO().KeyShift);
          v.x = vx;
          v.y = vy;
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

namespace {

// Where a floor's images live, matching how the backends already store them.
std::string layer_folder(const std::string &level_name) {
  return "layers/" + level_name;
}

// A bare filename goes under the floor's folder. A path the user spelled out
// with a folder in it is left exactly as typed.
std::string default_layer_path(const std::string &level_name,
                               const std::string &file) {
  if (file.empty() || file.find('/') != std::string::npos)
    return file;
  return layer_folder(level_name) + "/" + file;
}

bool under_layers_folder(const std::string &path) {
  return path.rfind("layers/", 0) == 0;
}

} // namespace

void EditorView::draw_add_layer_section(Building &building,
                                        EditorState &state) {
  Level &level = building.levels[state.level_idx];
  double mpp = compute_level_mpp(building, state.level_idx);

  if (state.open_add_layer_modal) {
    ImGui::OpenPopup("Add Layer");
    state.open_add_layer_modal = false;
  }
  if (ImGuiWidgets::BeginModal("Add Layer")) {
    if (ImGuiWidgets::BeginFormTable("##add_layer_form")) {
      ImGuiWidgets::FormRow("Name");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      ImGui::InputText("##name", &state.new_layer_name);
      ImGuiWidgets::FormRow("Filename");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      ImGui::InputText("##filename", &state.new_layer_filename);
      ImGui::EndTable();
    }
    ImGui::TextDisabled("Path is relative to the building.yaml directory.");
    const std::string will_be =
        default_layer_path(level.name, state.new_layer_filename);
    if (will_be != state.new_layer_filename)
      ImGui::TextDisabled("Goes to %s", will_be.c_str());
    const bool can_add =
        !state.new_layer_name.empty() && !state.new_layer_filename.empty();
    const int action = ImGuiWidgets::ModalActions("Add", "Cancel", can_add);
    if (action == 1) {
      Layer L;
      L.name = state.new_layer_name;
      L.filename = default_layer_path(level.name, state.new_layer_filename);
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
    } else if (action == 2) {
      ImGui::CloseCurrentPopup();
    }
    ImGuiWidgets::EndModal();
  }
}

namespace {
struct BrowseEntry {
  std::string name;
  bool is_dir = false;
};

std::string browse_json_str(const std::string &src, const std::string &key) {
  std::string needle = "\"" + key + "\":\"";
  auto pos = src.find(needle);
  if (pos == std::string::npos)
    return {};
  pos += needle.size();
  std::string out;
  while (pos < src.size() && src[pos] != '"') {
    if (src[pos] == '\\' && pos + 1 < src.size()) {
      out.push_back(src[pos + 1]);
      pos += 2;
    } else {
      out.push_back(src[pos++]);
    }
  }
  return out;
}
bool browse_json_bool(const std::string &src, const std::string &key) {
  std::string needle = "\"" + key + "\":";
  auto pos = src.find(needle);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t'))
    ++pos;
  return src.compare(pos, 4, "true") == 0;
}
std::vector<BrowseEntry> browse_parse(const std::string &payload) {
  std::vector<BrowseEntry> out;
  std::string marker = "\"entries\":[";
  auto ep = payload.find(marker);
  if (ep == std::string::npos)
    return out;
  ep += marker.size();
  while (true) {
    auto os = payload.find('{', ep);
    if (os == std::string::npos)
      break;
    auto oe = payload.find('}', os);
    if (oe == std::string::npos)
      break;
    std::string obj = payload.substr(os, oe - os + 1);
    BrowseEntry e;
    e.name = browse_json_str(obj, "name");
    e.is_dir = browse_json_bool(obj, "is_dir");
    if (!e.name.empty())
      out.push_back(e);
    ep = oe + 1;
  }
  return out;
}
} // namespace

void EditorView::draw_layer_browse_modal(Building &building,
                                         EditorState &state) {
  Level &level = building.levels[state.level_idx];
  if (state.open_layer_browse) {
    ImGui::OpenPopup("Browse layers");
    state.open_layer_browse = false;
    state.browse_relist = true;
    state.browse_status.clear();
    state.browse_last_upload.clear();
  }
  if (!ImGuiWidgets::BeginModal("Browse layers", 460.0f))
    return;

  if (state.browse_relist) {
    mev_assets_list(building_id_.c_str(), state.browse_subdir.c_str());
    state.browse_relist = false;
  }

  ImGui::TextDisabled("%s/%s", building_id_.c_str(),
                      state.browse_subdir.c_str());

  if (!state.browse_subdir.empty()) {
    if (ImGui::SmallButton(ICON_MDI_ARROW_UP " Up")) {
      auto slash = state.browse_subdir.find_last_of('/');
      state.browse_subdir = (slash == std::string::npos)
                                ? std::string()
                                : state.browse_subdir.substr(0, slash);
      state.browse_relist = true;
    }
    ImGui::SameLine();
  }
  if (ImGui::SmallButton(ICON_MDI_REFRESH " Refresh"))
    state.browse_relist = true;
  ImGui::SameLine();
  if (ImGui::SmallButton(ICON_MDI_UPLOAD " Upload"))
    mev_asset_upload(building_id_.c_str(), state.browse_subdir.c_str());

  {
    std::string uc = mev_asset_up_code();
    std::string un = mev_asset_up_name();
    if (uc == "busy") {
      state.browse_status = "Uploading " + un;
    } else if (uc == "ok" && un != state.browse_last_upload) {
      state.browse_last_upload = un;
      state.browse_status = "Uploaded " + un;
      state.browse_relist = true;
    } else if (uc == "err") {
      state.browse_status = "Upload failed: " + un;
    }
  }

  ImGui::Separator();
  std::string code = mev_assets_code();
  ImGui::BeginChild("##browse_list", ImVec2(0, 240), true);
  if (code == "busy") {
    ImGui::TextDisabled("Loading...");
  } else if (code == "err") {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.33f, 1.0f), "%s",
                       mev_assets_payload());
  } else if (code == "ok") {
    auto entries = browse_parse(mev_assets_payload());
    if (entries.empty())
      ImGui::TextDisabled("(empty)");
    for (const auto &e : entries) {
      ImGui::PushID(e.name.c_str());
      if (e.is_dir) {
        std::string lbl = std::string(ICON_MDI_FOLDER " ") + e.name;
        if (ImGui::Selectable(lbl.c_str())) {
          state.browse_subdir = state.browse_subdir.empty()
                                    ? e.name
                                    : state.browse_subdir + "/" + e.name;
          state.browse_relist = true;
        }
      } else {
        std::string lbl = std::string(ICON_MDI_FILE_IMAGE " ") + e.name;
        if (ImGui::Selectable(lbl.c_str())) {
          std::string rel = state.browse_subdir.empty()
                                ? e.name
                                : state.browse_subdir + "/" + e.name;
          if (state.selected_layer == -2) {
            level.drawing_filename = rel;
            yjs_op_drawing_set(level.name, rel);
            texture_provider_->clear_cache();
          } else if (state.selected_layer >= 0 &&
                     state.selected_layer < (int)level.layers.size()) {
            Layer &L = level.layers[state.selected_layer];
            L.filename = rel;
            yjs_op_layer_set(level.name, L);
          }
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::PopID();
    }
  } else {
    ImGui::TextDisabled("Click Refresh to list files.");
  }
  ImGui::EndChild();

  if (!state.browse_status.empty())
    ImGui::TextDisabled("%s", state.browse_status.c_str());
  if (ImGuiWidgets::ModalActions("Close"))
    ImGui::CloseCurrentPopup();
  ImGuiWidgets::EndModal();
}

void EditorView::draw_building_panel(Building &building, EditorState &state) {
  (void)state;
  ImGui::TextDisabled("Building");
  const std::string &cur = building.reference_level_name;
  const char *preview = !cur.empty() ? cur.c_str()
                        : building.levels.empty()
                            ? ""
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
  constexpr int kFloorplanSel = -2; // state.selected_layer sentinel: floorplan

  // Drop stale align target if a peer deleted the layer.
  if (state.align_layer_idx >= (int)level.layers.size())
    state.align_layer_idx = -1;

  if (!ImGui::CollapsingHeader("Layers"))
    return;

  double mpp = compute_level_mpp(building, state.level_idx);
  ImGui::AlignTextToFramePadding();
  if (mpp > 0.0)
    ImGui::TextDisabled("mpp %.4f", mpp);
  else
    ImGui::TextDisabled("no mpp");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Meters per image pixel for this level.\n"
                      "Computed from the level's measurement lines:\n"
                      "real length (m) divided by pixel length.");
  const char *add_lbl = ICON_MDI_PLUS " Add layer";
  float add_w =
      ImGui::CalcTextSize(add_lbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SameLine();
  float add_x = ImGui::GetContentRegionMax().x - add_w;
  if (add_x > ImGui::GetCursorPosX())
    ImGui::SetCursorPosX(add_x);
  if (ImGui::Button(add_lbl)) {
    state.open_add_layer_modal = true;
    state.new_layer_name.clear();
    state.new_layer_filename.clear();
  }

  {
    const Level &lvl = building.levels[state.level_idx];
    int stray = 0;
    if (!lvl.drawing_filename.empty() &&
        asset_path_is_portable(lvl.drawing_filename) &&
        !under_layers_folder(lvl.drawing_filename))
      ++stray;
    for (const Layer &L : lvl.layers) {
      if (!L.filename.empty() && asset_path_is_portable(L.filename) &&
          !under_layers_folder(L.filename))
        ++stray;
    }
    if (stray > 0) {
      ImGui::TextDisabled("%d image%s on this floor sit%s outside %s", stray,
                          stray == 1 ? "" : "s", stray == 1 ? "s" : "",
                          layer_folder(lvl.name).c_str());
    }
  }

  // A failed layer otherwise renders as nothing, with no hint why.
  auto missing_badge = [&](const std::string &cache_key,
                           const std::string &path) {
    if (!texture_provider_ ||
        texture_provider_->status_of(cache_key) != canvas::LoadStatus::Failed)
      return;
    ImGui::SameLine();
    ImGui::TextColored(theme::palette::danger, ICON_MDI_ALERT_CIRCLE);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("file not found: %s", path.c_str());
  };

  auto path_notes = [&](const std::string &cache_key, const std::string &path) {
    if (path.empty())
      return;
    if (texture_provider_ &&
        texture_provider_->status_of(cache_key) == canvas::LoadStatus::Failed) {
      ImGui::TextColored(theme::palette::danger,
                         ICON_MDI_ALERT_CIRCLE " file not found");
    }
    if (!asset_path_is_portable(path)) {
      ImGui::TextColored(theme::palette::warning,
                         ICON_MDI_ALERT_OUTLINE " outside the map folder");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Paths are relative to the building.yaml directory. "
                          "This one is not, so it breaks if the map moves and "
                          "it cannot round-trip through a .rmfmap bundle "
                          "unchanged.");
      }
    } else if (!under_layers_folder(path)) {
      // Said, not done. Moving an image means moving it in the backend too,
      // which is not this panel's business.
      ImGui::TextDisabled(ICON_MDI_FOLDER_ALERT " not under layers/");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Images normally live in %s, in the bundle and in "
            "the backend alike. This one would be %s. Nothing "
            "moves it for you.",
            layer_folder(level.name).c_str(),
            default_layer_path(level.name,
                               path.substr(path.find_last_of('/') + 1))
                .c_str());
      }
    }
  };

  if (ImGui::BeginTable("##layers_tbl", 1, ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);

    // Floorplan row, sentinel -2.
    ImGui::PushID("__fp_row");
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (ImGui::Selectable("Floorplan", state.selected_layer == kFloorplanSel)) {
      flush_pending_layer(level, state);
      state.selected_layer = kFloorplanSel;
    }
    missing_badge(canvas::floorplan_cache_key(level.name),
                  level.drawing_filename);
    ImGui::PopID();

    for (int i = 0; i < (int)level.layers.size(); ++i) {
      Layer &L = level.layers[i];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const bool aligning = (state.align_layer_idx == i);
      if (aligning)
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::success);
      if (ImGui::Selectable(L.name.c_str(), state.selected_layer == i)) {
        flush_pending_layer(level, state);
        state.selected_layer = i;
      }
      if (aligning)
        ImGui::PopStyleColor();
      missing_badge(canvas::layer_cache_key(level.name, L.name), L.filename);
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (state.align_layer_idx >= 0 &&
      state.align_layer_idx < (int)level.layers.size()) {
    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Aligning: %s",
                       level.layers[state.align_layer_idx].name.c_str());
    ImGui::TextDisabled("Wheel = scale, drag = move, Ctrl = fine");
  }

  if (state.selected_layer == kFloorplanSel) {
    ImGui::Separator();
    ImGui::TextUnformatted("Floorplan");
    if (ImGuiWidgets::BeginFormTable("##fp_edit")) {
      ImGuiWidgets::FormRow("Path");
      float browse_w = ImGui::CalcTextSize(ICON_MDI_FOLDER_OPEN).x +
                       ImGui::GetStyle().FramePadding.x * 2.0f;
      float path_w = ImGuiWidgets::FormControlWidth() - browse_w -
                     ImGui::GetStyle().ItemSpacing.x;
      if (path_w < 40.0f)
        path_w = 40.0f;
      ImGui::SetNextItemWidth(path_w);
      ImGui::InputText("##fppath", &level.drawing_filename);
      const bool fp_commit = ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      if (ImGui::Button(ICON_MDI_FOLDER_OPEN "##fpbrowse")) {
        state.open_layer_browse = true;
        auto slash = level.drawing_filename.find_last_of('/');
        state.browse_subdir = (slash == std::string::npos)
                                  ? layer_folder(level.name)
                                  : level.drawing_filename.substr(0, slash);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Browse files");
      ImGui::EndTable();
      path_notes(canvas::floorplan_cache_key(level.name),
                 level.drawing_filename);
      if (fp_commit) {
        yjs_op_drawing_set(level.name, level.drawing_filename);
        texture_provider_->clear_cache();
      }
    }
  }

  if (state.selected_layer >= 0 &&
      state.selected_layer < (int)level.layers.size()) {
    ImGui::Separator();
    Layer &L = level.layers[state.selected_layer];
    const std::string layer_name = L.name;
    // Keep the rename buffer in sync with the selected layer. It holds the
    // in-progress name so L.name stays the old CRDT key until commit.
    if (state.layer_name_buf_idx != state.selected_layer) {
      state.layer_name_buf = L.name;
      state.layer_name_buf_idx = state.selected_layer;
    }
    bool l_dirty = false, l_commit = false;
    if (ImGuiWidgets::BeginFormTable("##layer_edit")) {
      ImGuiWidgets::FormRow("Name");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      ImGui::InputText("##lname", &state.layer_name_buf);
      const bool name_active = ImGui::IsItemActive();
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string nn = state.layer_name_buf;
        bool taken = nn.empty();
        for (int j = 0; !taken && j < (int)level.layers.size(); ++j)
          if (j != state.selected_layer && level.layers[j].name == nn)
            taken = true;
        if (!taken && nn != layer_name) {
#ifdef __EMSCRIPTEN__
          yjs_op_layer_delete(level.name, layer_name); // rename = drop old key
#endif
          L.name = nn;
          yjs_op_layer_set(level.name, L);
        } else {
          state.layer_name_buf = layer_name; // revert empty / duplicate
        }
      } else if (!name_active && state.layer_name_buf != L.name) {
        // Resync when idle: a delete/reindex may have reused this index.
        state.layer_name_buf = L.name;
      }
      ImGuiWidgets::FormRow("Path");
      float browse_w = ImGui::CalcTextSize(ICON_MDI_FOLDER_OPEN).x +
                       ImGui::GetStyle().FramePadding.x * 2.0f;
      float path_w = ImGuiWidgets::FormControlWidth() - browse_w -
                     ImGui::GetStyle().ItemSpacing.x;
      if (path_w < 40.0f)
        path_w = 40.0f;
      ImGui::SetNextItemWidth(path_w);
      if (ImGui::InputText("##lpath", &L.filename))
        l_dirty = true;
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGui::SameLine();
      if (ImGui::Button(ICON_MDI_FOLDER_OPEN "##browse")) {
        state.open_layer_browse = true;
        auto slash = L.filename.find_last_of('/');
        state.browse_subdir = (slash == std::string::npos)
                                  ? layer_folder(level.name)
                                  : L.filename.substr(0, slash);
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Browse files");
      float scale = (float)L.scale;
      float yaw_deg = (float)(L.yaw * 180.0 / M_PI);
      float tx = (float)L.translation_x;
      float ty = (float)L.translation_y;
      ImGuiWidgets::FormRow("Scale m/px");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      if (ImGui::DragFloat("##lscale", &scale, 0.0005f, 0.0001f, 100.0f,
                           "%.6f")) {
        L.scale = scale;
        l_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGuiWidgets::FormRow("Yaw deg");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      if (ImGui::DragFloat("##lyaw", &yaw_deg, 0.5f, -360.0f, 360.0f, "%.2f")) {
        L.yaw = yaw_deg * M_PI / 180.0;
        l_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGuiWidgets::FormRow("Translation m");
      float half =
          (ImGuiWidgets::FormControlWidth() - ImGui::GetStyle().ItemSpacing.x) *
          0.5f;
      if (half < 1.0f)
        half = 1.0f;
      ImGui::SetNextItemWidth(half);
      if (ImGui::DragFloat("##ltx", &tx, 0.05f)) {
        L.translation_x = tx;
        l_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(half);
      if (ImGui::DragFloat("##lty", &ty, 0.05f)) {
        L.translation_y = ty;
        l_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGuiWidgets::FormRow("Color");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      float col4[4] = {(float)L.color_r, (float)L.color_g, (float)L.color_b,
                       (float)L.color_a};
      if (ImGui::ColorEdit4("##lcol", col4,
                            ImGuiColorEditFlags_NoInputs |
                                ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_AlphaPreviewHalf)) {
        L.color_r = col4[0];
        L.color_g = col4[1];
        L.color_b = col4[2];
        L.color_a = col4[3];
        l_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        l_commit = true;
      ImGui::EndTable();
      path_notes(canvas::layer_cache_key(level.name, L.name), L.filename);
    }

    const float full = ImGui::GetContentRegionAvail().x;
    {
      const bool aligning = (state.align_layer_idx == state.selected_layer);
      if (aligning)
        ImGui::PushStyleColor(ImGuiCol_Button, theme::palette::blue);
      if (ImGui::Button(aligning ? "Done aligning" : "Align on map",
                        ImVec2(full, 0.0f))) {
        flush_pending_layer(level, state);
        state.align_layer_idx = aligning ? -1 : state.selected_layer;
      }
      if (aligning)
        ImGui::PopStyleColor();
    }

    bool deleted = false;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          theme::with_alpha(theme::palette::danger, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::palette::danger);
    if (ImGui::Button("Delete layer", ImVec2(full, 0.0f)))
      ImGui::OpenPopup("Delete layer##del_layer");
    ImGui::PopStyleColor(2);
    if (ImGuiWidgets::BeginModal("Delete layer##del_layer", 320.0f)) {
      ImGui::Text("Delete layer \"%s\"?", layer_name.c_str());
      const int a = ImGuiWidgets::ModalActions("Delete", "Cancel");
      if (a == 1) {
        const int i = state.selected_layer;
#ifdef __EMSCRIPTEN__
        yjs_op_layer_delete(level.name, layer_name);
#endif
        level.layers.erase(level.layers.begin() + i);
        state.selected_layer = -1;
        if (state.align_layer_idx == i)
          state.align_layer_idx = -1;
        else if (state.align_layer_idx > i)
          state.align_layer_idx -= 1;
        deleted = true;
        ImGui::CloseCurrentPopup();
      } else if (a == 2) {
        ImGui::CloseCurrentPopup();
      }
      ImGuiWidgets::EndModal();
    }

    if (!deleted) {
      if (l_dirty) {
        state.pending_commit_layer = layer_name;
        state.pending_commit_time = ImGui::GetTime();
      }
      if (l_commit) {
#ifdef __EMSCRIPTEN__
        yjs_op_layer_set(level.name, L);
#endif
        state.pending_commit_layer.clear();
        state.pending_commit_time = 0.0;
      }
    }
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

void EditorView::draw_mutex_groups_panel(Building &building,
                                         EditorState &state) {
  if (!ImGui::CollapsingHeader("Mutex Groups"))
    return;
  Level &level = building.levels[state.level_idx];
  std::vector<MutexGroupInfo> groups = gather_mutex_groups(level);

  auto set_on_selection = [&](const std::string &grp) {
    for (int li : state.selected_lanes)
      if (li >= 0 && li < (int)level.lanes.size()) {
        level.lanes[li].params["mutex"] = ParamValue::make_string(grp);
        yjs_op_lane_replace(level.name, li, level.lanes[li]);
      }
    for (int vi : state.selected_vertices)
      if (vi >= 0 && vi < (int)level.vertices.size()) {
        level.vertices[vi].params["mutex"] = ParamValue::make_string(grp);
        yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
      }
  };
  auto retag_group = [&](const std::string &from, const std::string &to) {
    for (int i = 0; i < (int)level.lanes.size(); ++i)
      if (get_mutex(level.lanes[i].params) == from) {
        level.lanes[i].params["mutex"] = ParamValue::make_string(to);
        yjs_op_lane_replace(level.name, i, level.lanes[i]);
      }
    for (int i = 0; i < (int)level.vertices.size(); ++i)
      if (get_mutex(level.vertices[i].params) == from) {
        level.vertices[i].params["mutex"] = ParamValue::make_string(to);
        yjs_op_vertex_replace(level.name, i, level.vertices[i]);
      }
  };

  const int nl = (int)state.selected_lanes.size();
  const int nv = (int)state.selected_vertices.size();
  const bool has_sel = (nl + nv) > 0;
  const ImGuiStyle &st = ImGui::GetStyle();

  if (groups.empty()) {
    ImGui::TextDisabled("No groups yet.");
  } else {
    for (const auto &g : groups) {
      ImGui::PushID(g.name.c_str());
      const bool active = (state.active_mutex_group == g.name);
      // Leading spaces make room for the swatch, drawn inside the selectable.
      std::string label = "     " + g.name + "   " + std::to_string(g.lanes) +
                          " lanes, " + std::to_string(g.vertices) + " verts";
      const float avail = ImGui::GetContentRegionAvail().x;
      const ImU32 sw_col = (ImU32)ImColor(canvas::mutex_color(g.name));
      const ImVec2 rowp = ImGui::GetCursorScreenPos();
      bool clicked = false;
      if (active) {
        const float clear_w =
            ImGui::CalcTextSize("Clear").x + st.FramePadding.x * 2.0f;
        float sel_w = avail - clear_w - st.ItemSpacing.x;
        if (sel_w < 1.0f)
          sel_w = 1.0f;
        clicked =
            ImGui::Selectable(label.c_str(), true, 0, ImVec2(sel_w, 0.0f));
      } else {
        clicked = ImGui::Selectable(label.c_str(), false);
      }
      {
        const float lh = ImGui::GetTextLineHeight();
        const float s = 11.0f;
        const float sy = rowp.y + (lh - s) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(rowp.x + 4.0f, sy), ImVec2(rowp.x + 4.0f + s, sy + s),
            sw_col, 2.0f);
      }
      if (active) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
          state.active_mutex_group.clear();
      }
      if (clicked) {
        if (active) {
          state.active_mutex_group.clear();
        } else {
          state.active_mutex_group = g.name;
          state.mutex_rename_buf = g.name;
        }
      }
      ImGui::PopID();
    }
  }

  ImGui::Separator();

  if (!state.active_mutex_group.empty()) {
    const std::string g = state.active_mutex_group;
    if (ImGuiWidgets::BeginFormTable("##mutex_manage")) {
      ImGuiWidgets::FormRow("Name");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      ImGui::InputText("##mrename", &state.mutex_rename_buf);
      if (ImGui::IsItemDeactivatedAfterEdit() &&
          !state.mutex_rename_buf.empty() && state.mutex_rename_buf != g) {
        retag_group(g, state.mutex_rename_buf);
        state.active_mutex_group = state.mutex_rename_buf;
      }
      ImGui::EndTable();
    }
    // Add/remove only the relevant items, a mixed selection enables both.
    auto add_to = [&](const std::string &grp) {
      for (int li : state.selected_lanes)
        if (li >= 0 && li < (int)level.lanes.size() &&
            get_mutex(level.lanes[li].params) != grp) {
          level.lanes[li].params["mutex"] = ParamValue::make_string(grp);
          yjs_op_lane_replace(level.name, li, level.lanes[li]);
        }
      for (int vi : state.selected_vertices)
        if (vi >= 0 && vi < (int)level.vertices.size() &&
            get_mutex(level.vertices[vi].params) != grp) {
          level.vertices[vi].params["mutex"] = ParamValue::make_string(grp);
          yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
        }
    };
    auto remove_from = [&](const std::string &grp) {
      for (int li : state.selected_lanes)
        if (li >= 0 && li < (int)level.lanes.size() &&
            get_mutex(level.lanes[li].params) == grp) {
          level.lanes[li].params["mutex"] = ParamValue::make_string("");
          yjs_op_lane_replace(level.name, li, level.lanes[li]);
        }
      for (int vi : state.selected_vertices)
        if (vi >= 0 && vi < (int)level.vertices.size() &&
            get_mutex(level.vertices[vi].params) == grp) {
          level.vertices[vi].params["mutex"] = ParamValue::make_string("");
          yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
        }
    };
    int addable = 0, removable = 0;
    for (int li : state.selected_lanes)
      if (li >= 0 && li < (int)level.lanes.size())
        (get_mutex(level.lanes[li].params) == g ? removable : addable)++;
    for (int vi : state.selected_vertices)
      if (vi >= 0 && vi < (int)level.vertices.size())
        (get_mutex(level.vertices[vi].params) == g ? removable : addable)++;

    if (has_sel)
      ImGui::TextDisabled("%d to add, %d to remove", addable, removable);
    else
      ImGui::TextDisabled("Select lanes/vertices to assign");
    const float full = ImGui::GetContentRegionAvail().x;
    ImGui::BeginDisabled(addable == 0);
    if (ImGui::Button("Add selection to group", ImVec2(full, 0.0f)))
      add_to(g);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(removable == 0);
    if (ImGui::Button("Remove selection from group", ImVec2(full, 0.0f)))
      remove_from(g);
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,
                          theme::with_alpha(theme::palette::danger, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::palette::danger);
    if (ImGui::Button("Delete group", ImVec2(full, 0.0f)))
      ImGui::OpenPopup("Delete group##del_mutex");
    ImGui::PopStyleColor(2);
    if (ImGuiWidgets::BeginModal("Delete group##del_mutex", 320.0f)) {
      ImGui::Text("Delete group \"%s\"?", g.c_str());
      ImGui::TextDisabled("This clears the group from all its members.");
      const int a = ImGuiWidgets::ModalActions("Delete", "Cancel");
      if (a == 1) {
        retag_group(g, "");
        state.active_mutex_group.clear();
        ImGui::CloseCurrentPopup();
      } else if (a == 2) {
        ImGui::CloseCurrentPopup();
      }
      ImGuiWidgets::EndModal();
    }
  } else if (has_sel) {
    ImGui::TextDisabled("%d lanes, %d vertices selected", nl, nv);
    const float full = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(full);
    ImGui::InputTextWithHint("##newgroup", "new group name",
                             &state.mutex_new_buf);
    ImGui::BeginDisabled(state.mutex_new_buf.empty());
    if (ImGui::Button("Create group from selection", ImVec2(full, 0.0f))) {
      set_on_selection(state.mutex_new_buf);
      state.active_mutex_group = state.mutex_new_buf;
      state.mutex_rename_buf = state.mutex_new_buf;
      state.mutex_new_buf.clear();
    }
    ImGui::EndDisabled();
  } else {
    ImGui::TextDisabled("Select lanes/vertices, then create or pick a group.");
  }
}

// Edit one param across several elements, (mixed) when they differ.
static void draw_multi_param_editor(
    const std::vector<std::map<std::string, ParamValue> *> &maps,
    const char *key, ParamType type, bool &commit) {
  if (maps.empty())
    return;
  auto write_all = [&](const ParamValue &pv) {
    for (auto *m : maps)
      (*m)[key] = pv;
    commit = true;
  };
  ImGui::PushID(key);
  ImGuiWidgets::FormRow(key);

  if (param_optional(key)) {
    int present = 0;
    ParamValue fp{};
    bool have = false, val_mixed = false;
    for (auto *m : maps) {
      auto it = m->find(key);
      if (it == m->end() || it->second.type != type)
        continue;
      ++present;
      if (!have) {
        fp = it->second;
        have = true;
      } else if (type == ParamType::INT ? it->second.i != fp.i
                                        : it->second.d != fp.d) {
        val_mixed = true;
      }
    }
    if (present == 0) {
      if (ImGui::Button("Set"))
        write_all(type == ParamType::INT ? ParamValue::make_int(0)
                                         : ParamValue::make_double(0.0));
      ImGui::PopID();
      return;
    }
    const bool mixed = val_mixed || present != (int)maps.size();
    const float clear_w = ImGui::GetFrameHeight();
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const float tag_w = mixed ? ImGui::CalcTextSize("(mixed)").x + sp : 0.0f;
    float in_w = ImGuiWidgets::FormControlWidth() - clear_w - sp - tag_w;
    if (in_w < 30.0f)
      in_w = 30.0f;
    ImGui::SetNextItemWidth(in_w);
    if (type == ParamType::INT) {
      int v = mixed ? 0 : fp.i;
      ImGui::InputInt("##v", &v);
      if (ImGui::IsItemDeactivatedAfterEdit())
        write_all(ParamValue::make_int(v));
    } else {
      float v = mixed ? 0.0f : (float)fp.d;
      ImGui::InputFloat("##v", &v, 0.0f, 0.0f, "%.4f");
      if (ImGui::IsItemDeactivatedAfterEdit())
        write_all(ParamValue::make_double((double)v));
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_MDI_CLOSE, ImVec2(clear_w, 0.0f))) {
      for (auto *m : maps)
        m->erase(key);
      commit = true;
    }
    if (mixed) {
      ImGui::SameLine();
      ImGui::TextDisabled("(mixed)");
    }
    ImGui::PopID();
    return;
  }

  auto read = [&](std::map<std::string, ParamValue> *m) {
    auto it = m->find(key);
    ParamValue pv = (it != m->end()) ? it->second : ParamValue{};
    if (pv.type != type) {
      pv = ParamValue{};
      pv.type = type;
    }
    return pv;
  };
  ParamValue first = read(maps[0]);
  bool mixed = false;
  for (size_t i = 1; i < maps.size() && !mixed; ++i) {
    ParamValue pv = read(maps[i]);
    switch (type) {
    case ParamType::BOOL:
      mixed = pv.b != first.b;
      break;
    case ParamType::INT:
      mixed = pv.i != first.i;
      break;
    case ParamType::DOUBLE:
      mixed = pv.d != first.d;
      break;
    default:
      mixed = pv.s != first.s;
      break;
    }
  }

  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  switch (type) {
  case ParamType::BOOL: {
    if (mixed) {
      int cur = 0;
      const char *opts[] = {"(mixed)", "false", "true"};
      if (ImGui::Combo("##v", &cur, opts, 3) && cur != 0)
        write_all(ParamValue::make_bool(cur == 2));
    } else {
      bool v = first.b;
      if (ImGui::Checkbox("##v", &v))
        write_all(ParamValue::make_bool(v));
    }
    break;
  }
  case ParamType::INT: {
    int v = mixed ? 0 : first.i;
    ImGui::InputInt("##v", &v);
    if (ImGui::IsItemDeactivatedAfterEdit())
      write_all(ParamValue::make_int(v));
    if (mixed) {
      ImGui::SameLine();
      ImGui::TextDisabled("(mixed)");
    }
    break;
  }
  case ParamType::DOUBLE: {
    float v = mixed ? 0.0f : (float)first.d;
    ImGui::InputFloat("##v", &v, 0.0f, 0.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit())
      write_all(ParamValue::make_double(v));
    if (mixed) {
      ImGui::SameLine();
      ImGui::TextDisabled("(mixed)");
    }
    break;
  }
  case ParamType::STRING: {
    std::string v = mixed ? std::string() : first.s;
    if (mixed)
      ImGui::InputTextWithHint("##v", "(mixed)", &v);
    else
      ImGui::InputText("##v", &v);
    if (ImGui::IsItemDeactivatedAfterEdit() && (!mixed || !v.empty()))
      write_all(ParamValue::make_string(v));
    break;
  }
  }
  ImGui::PopID();
}

static void draw_multi_orientation_combo(
    const std::vector<std::map<std::string, ParamValue> *> &maps,
    bool &commit) {
  if (maps.empty())
    return;
  auto orient_of = [&](std::map<std::string, ParamValue> *m) -> int {
    auto it = m->find("orientation");
    if (it == m->end() || it->second.type != ParamType::STRING)
      return 0;
    if (it->second.s == "forward")
      return 1;
    if (it->second.s == "backward")
      return 2;
    return 0;
  };
  int common = orient_of(maps[0]);
  bool mixed = false;
  for (size_t i = 1; i < maps.size() && !mixed; ++i)
    if (orient_of(maps[i]) != common)
      mixed = true;
  auto write_all = [&](const std::string &val) {
    for (auto *m : maps)
      (*m)["orientation"] = ParamValue::make_string(val);
    commit = true;
  };
  ImGui::PushID("multi_orient");
  ImGuiWidgets::FormRow("orientation");
  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  if (mixed) {
    const char *opts[] = {"(mixed)", "(none)", "forward", "backward"};
    int cur = 0;
    if (ImGui::Combo("##v", &cur, opts, 4) && cur != 0)
      write_all(cur == 2 ? "forward" : cur == 3 ? "backward" : "");
  } else {
    const char *opts[] = {"(none)", "forward", "backward"};
    int cur = common;
    if (ImGui::Combo("##v", &cur, opts, 3))
      write_all(cur == 1 ? "forward" : cur == 2 ? "backward" : "");
  }
  ImGui::PopID();
}

void EditorView::draw_attribute_panel(Building &building, EditorState &state) {
  Level &level = building.levels[state.level_idx];
  const std::vector<MutexGroupInfo> mutex_groups = gather_mutex_groups(level);

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
  flush_pending_layer(level, state);
  if (state.pending_commit_vertex >= 0 &&
      state.pending_commit_vertex != single_vertex)
    flush_pending_vertex(level, state);
  if (state.pending_commit_lane >= 0 &&
      state.pending_commit_lane != single_lane)
    flush_pending_lane(level, state);

  if (!ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  bool emitted = false;
  if (state.selected_vertices.size() > 1) {
    std::vector<int> sel;
    for (int vi : state.selected_vertices)
      if (vi >= 0 && vi < (int)level.vertices.size())
        sel.push_back(vi);
    if (!sel.empty()) {
      ImGui::Text("%d vertices selected", (int)sel.size());
      ImGui::Separator();
      std::vector<std::map<std::string, ParamValue> *> maps;
      for (int vi : sel)
        maps.push_back(&level.vertices[vi].params);
      bool v_commit = false;
      if (ImGuiWidgets::BeginFormTable("##vmulti")) {
        for (const auto &spec : kVertexParams) {
          if (std::strcmp(spec.key, "mutex") == 0)
            continue;
          draw_multi_param_editor(maps, spec.key, spec.type, v_commit);
        }
        ImGui::EndTable();
      }
      if (v_commit)
        for (int vi : sel)
          yjs_op_vertex_replace(level.name, vi, level.vertices[vi]);
    }
    emitted = true;
  } else if (single_vertex >= 0) {
    int vi = single_vertex;
    Vertex &v = level.vertices[vi];
    ImGui::Text("Vertex #%d", vi);
    ImGui::Separator();
    bool v_dirty = false, v_commit = false;
    if (ImGuiWidgets::BeginFormTable("##vprops")) {
      ImGuiWidgets::FormRow("Name");
      ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
      if (ImGui::InputText("##vname", &v.name))
        v_dirty = true;
      if (ImGui::IsItemDeactivatedAfterEdit())
        v_commit = true;

      ImGuiWidgets::FormRow("Position (x, y) m");
      float xf = (float)v.x, yf = (float)v.y;
      float half =
          (ImGuiWidgets::FormControlWidth() - ImGui::GetStyle().ItemSpacing.x) *
          0.5f;
      if (half < 1.0f)
        half = 1.0f;
      ImGui::SetNextItemWidth(half);
      if (ImGui::InputFloat("##vx", &xf, 0.0f, 0.0f, "%.3f")) {
        v.x = xf;
        v_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        v_commit = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(half);
      if (ImGui::InputFloat("##vy", &yf, 0.0f, 0.0f, "%.3f")) {
        v.y = yf;
        v_dirty = true;
      }
      if (ImGui::IsItemDeactivatedAfterEdit())
        v_commit = true;

      for (const auto &spec : kVertexParams) {
        if (std::strcmp(spec.key, "mutex") == 0)
          continue;
        draw_param_row(v.params, spec.key, spec.type, v_dirty, v_commit);
      }
      draw_mutex_row(v.params, mutex_groups, state, v_dirty, v_commit);
      ImGui::EndTable();
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
    if (ImGuiWidgets::BeginFormTable("##lprops")) {
      for (const auto &spec : kLaneParams) {
        if (std::strcmp(spec.key, "orientation") == 0 ||
            std::strcmp(spec.key, "mutex") == 0)
          continue;
        draw_param_row(l.params, spec.key, spec.type, ln_dirty, ln_commit);
      }
      draw_orientation_row(l.params, ln_dirty, ln_commit);
      draw_mutex_row(l.params, mutex_groups, state, ln_dirty, ln_commit);
      ImGui::EndTable();
    }
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
      ImGui::Text("%d lanes selected", (int)sel.size());
      ImGui::Separator();
      for (int li : sel)
        init_default_lane_params(level.lanes[li]);
      std::vector<std::map<std::string, ParamValue> *> maps;
      for (int li : sel)
        maps.push_back(&level.lanes[li].params);
      bool ln_commit = false;
      if (ImGuiWidgets::BeginFormTable("##lmulti")) {
        for (const auto &spec : kLaneParams) {
          if (std::strcmp(spec.key, "orientation") == 0 ||
              std::strcmp(spec.key, "mutex") == 0)
            continue;
          draw_multi_param_editor(maps, spec.key, spec.type, ln_commit);
        }
        draw_multi_orientation_combo(maps, ln_commit);
        ImGui::EndTable();
      }
      if (ln_commit)
        for (int li : sel)
          yjs_op_lane_replace(level.name, li, level.lanes[li]);
      emitted = true;
    }
  }

  if (!emitted && state.selected_walls.size() == 1 &&
      state.selected_walls[0] >= 0 &&
      state.selected_walls[0] < (int)level.walls.size()) {
    int wi = state.selected_walls[0];
    Wall &w = level.walls[wi];
    ImGui::Text("Wall #%d  (%d -> %d)", wi, w.start_idx, w.end_idx);
    ImGui::Separator();
    init_default_wall_params(w);
    bool d = false, c = false;
    for (const auto &spec : kWallParams)
      draw_param_editor(w.params, spec.key, spec.type, d, c);
    if (c)
      yjs_op_wall_replace(level.name, wi, w);
    emitted = true;
  }

  if (!emitted && state.selected_doors.size() == 1 &&
      state.selected_doors[0] >= 0 &&
      state.selected_doors[0] < (int)level.doors.size()) {
    int di = state.selected_doors[0];
    Door &dr = level.doors[di];
    ImGui::Text("Door #%d  (%d -> %d)", di, dr.start_idx, dr.end_idx);
    ImGui::Separator();
    init_default_door_params(dr);
    bool d = false, c = false;
    {
      auto &pv = dr.params["type"];
      if (pv.type != ParamType::STRING) {
        pv.type = ParamType::STRING;
        pv.s = "hinged";
      }
      const char *types[] = {"hinged", "double_hinged", "sliding",
                             "double_sliding"};
      int cur = 0;
      for (int k = 0; k < 4; ++k)
        if (pv.s == types[k])
          cur = k;
      ImGui::PushID("door_type");
      if (ImGui::Combo("type", &cur, types, 4)) {
        pv.s = types[cur];
        c = true;
      }
      ImGui::PopID();
    }
    {
      auto &pv = dr.params["motion_axis"];
      if (pv.type != ParamType::STRING) {
        pv.type = ParamType::STRING;
        pv.s = "start";
      }
      const char *ax[] = {"start", "end"};
      int cur = (pv.s == "end") ? 1 : 0;
      ImGui::PushID("door_axis");
      if (ImGui::Combo("motion_axis", &cur, ax, 2)) {
        pv.s = ax[cur];
        c = true;
      }
      ImGui::PopID();
    }
    for (const auto &spec : kDoorParams)
      draw_param_editor(dr.params, spec.key, spec.type, d, c);
    if (c)
      yjs_op_door_replace(level.name, di, dr);
    emitted = true;
  }

  if (!emitted && state.selected_measurements.size() == 1 &&
      state.selected_measurements[0] >= 0 &&
      state.selected_measurements[0] < (int)level.measurements.size()) {
    int mi = state.selected_measurements[0];
    Measurement &m = level.measurements[mi];
    ImGui::Text("Measurement #%d  (%d -> %d)", mi, m.start_idx, m.end_idx);
    ImGui::Separator();
    init_default_measurement_params(m);
    bool d = false, c = false;
    draw_param_editor(m.params, "distance", ParamType::DOUBLE, d, c);
    if (c)
      yjs_op_measurement_replace(level.name, mi, m);
    double mpp = compute_level_mpp(building, state.level_idx);
    if (mpp > 0.0)
      ImGui::TextDisabled("level scale: %.5f m/px", mpp);
    emitted = true;
  }

  if (!emitted && state.selected_floor >= 0 &&
      state.selected_floor < (int)level.floors.size()) {
    int fi = state.selected_floor;
    Floor &f = level.floors[fi];
    ImGui::Text("Floor #%d  (%d vertices, %d holes)", fi,
                (int)f.vertices.size(), (int)f.holes.size());
    ImGui::Separator();
    init_default_floor_params(f);
    bool d = false, c = false;
    for (const auto &spec : kFloorParams)
      draw_param_editor(f.params, spec.key, spec.type, d, c);
    if (c)
      yjs_op_floor_replace(level.name, fi, f);
    ImGui::TextDisabled("Hole mode [H] adds a hole to this floor.");
    emitted = true;
  }

  if (!emitted) {
    ImGui::TextDisabled("No selection.");
  }
}

void EditorView::draw_status_bar(const EditorState &state) {
  (void)state;

#ifdef __EMSCRIPTEN__
  const char *yjs = map_editor_yjs_status();
  if (yjs && yjs[0]) {
    bool ok = std::strcmp(yjs, "connected") == 0 && map_editor_yjs_synced();
    const theme::Signal sig = ok ? theme::Signal::success
                              : std::strcmp(yjs, "connecting") == 0
                                  ? theme::Signal::warning
                                  : theme::Signal::danger;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::signal_color(sig));
    ImGui::Text("collab: %s%s", yjs, ok ? " (synced)" : "");
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
    if (lvl.drawing_filename.empty())
      return;
    tex = &provider.acquire("fp:" + lvl.name, asset_id, lvl.drawing_filename,
                            1.0, 1.0, 1.0);
    s_w = 1.0;
  } else {
    const Layer *L = nullptr;
    for (const Layer &q : lvl.layers)
      if (q.name == image_name) {
        L = &q;
        break;
      }
    if (!L)
      return;
    tex = &provider.acquire("lay:" + lvl.name + ":" + L->name, asset_id,
                            L->filename, L->color_r, L->color_g, L->color_b);
    s_w = L->scale / level_mpp;
    cosL = std::cos(L->yaw);
    sinL = std::sin(L->yaw);
    lx = L->translation_x / level_mpp;
    ly = L->translation_y / level_mpp;
  }
  if (!tex || tex->status != canvas::LoadStatus::Ok)
    return;
  ow = tex->orig_width > 0 ? tex->orig_width : tex->width;
  oh = tex->orig_height > 0 ? tex->orig_height : tex->height;
  auto corner = [&](double ix, double iy) {
    double a = ix * s_w, b = iy * s_w;
    double wx = lx + cosL * a - sinL * b;
    double wy = ly + sinL * a + cosL * b;
    if (xf) {
      auto p = tgt_to_ref(*xf, wx, wy);
      wx = p.first;
      wy = p.second;
    }
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
  if (tgt <= 0 || tgt >= (int)building.levels.size())
    tgt = 1;
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
      if (sel)
        ImGui::SetItemDefaultFocus();
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
    ImGui::TextDisabled(
        "translation only — scroll-zoom is a no-op until 2+ pairs");
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
      ImGui::TextColored(sel ? ImVec4(1, 1, 1, 1) : col, "%s  (%.0f, %.0f)",
                         f.name.c_str(), f.x, f.y);
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
  if (ref_mpp <= 0.0)
    ref_mpp = 1.0;
  double tgt_mpp = ref_mpp;
  if (has_target) {
    xf = compute_floor_transform(building.levels[0].fiducials,
                                 building.levels[tgt_idx].fiducials,
                                 default_target_scale(building, tgt_idx));
    if (state.align_floors_tgt_mpp <= 0.0)
      state.align_floors_tgt_mpp = compute_level_mpp(building, tgt_idx);
    if (state.align_floors_tgt_mpp > 0.0)
      tgt_mpp = state.align_floors_tgt_mpp;
  }

  canvas::DrawOptions opts;
  opts.show_vertex_names = false;
  opts.draw_floorplan = false;
  opts.draw_layers = false;
  opts.draw_vertices = false;
  opts.draw_lanes = false;
  opts.after_draw = [&, xf, tgt_mpp, ref_mpp,
                     has_target](const canvas::MapCanvas &c) {
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
  ImGui::InvisibleButton("##align_floors_canvas", canvas_.canvas_size(),
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered();
  handle_floor_align_input(building, state, hovered);
}

void EditorView::handle_floor_align_input(Building &building,
                                          EditorState &state, bool hovered) {
  if (!state.align_floors_mode)
    return;
  const int tgt = state.align_floors_target;
  if (tgt <= 0 || tgt >= (int)building.levels.size())
    return;
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
    if (level == 0)
      return std::pair<double, double>{cx, cy};
    return ref_to_tgt(xf, cx, cy);
  };
  auto floor_to_canvas = [&](int level, double fx, double fy) {
    if (level == 0)
      return std::pair<double, double>{fx, fy};
    return tgt_to_ref(xf, fx, fy);
  };

  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape))
    state.align_floors_placing = false;

  static bool s_mpan = false;
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    s_mpan = true;
  if (s_mpan) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      canvas_.view_state().offset_x += io.MouseDelta.x;
      canvas_.view_state().offset_y += io.MouseDelta.y;
    } else
      s_mpan = false;
  }

  if (hovered && io.MouseWheel != 0.0f && !target.fiducials.empty() &&
      xf.scale > 1e-12) {
    double factor = 1.0 - (double)io.MouseWheel * 0.05;
    if (factor < 0.1)
      factor = 0.1;
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
    out_level = -1;
    out_idx = -1;
    float best = kFidHitPx;
    for (int i = 0; i < (int)ref.fiducials.size(); ++i) {
      auto [wx, wy] =
          floor_to_canvas(0, ref.fiducials[i].x, ref.fiducials[i].y);
      ImVec2 p = canvas_.world_to_screen(wx, wy);
      float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
      if (d < best) {
        best = d;
        out_level = 0;
        out_idx = i;
      }
    }
    for (int i = 0; i < (int)target.fiducials.size(); ++i) {
      auto [wx, wy] =
          floor_to_canvas(tgt, target.fiducials[i].x, target.fiducials[i].y);
      ImVec2 p = canvas_.world_to_screen(wx, wy);
      float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
      if (d < best) {
        best = d;
        out_level = tgt;
        out_idx = i;
      }
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
      rf.name = name;
      rf.x = rfx;
      rf.y = rfy;
      tf.name = name;
      tf.x = tfx;
      tf.y = tfy;
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
      } else if (lvl == tgt && idx >= 0 && idx < (int)target.fiducials.size() &&
                 xf.scale > 1e-12) {
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
      for (Fiducial &f : target.fiducials) {
        f.x -= dix;
        f.y -= diy;
      }
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

void EditorView::draw_version_strip(EditorState &state,
                                    const TopBarHooks &top_bar) {
  const bool on_snapshot = !state.snapshot_dir.empty();
  // Pull up over the child spacing so no bg sliver shows above the bar.
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                       ImGui::GetStyle().ItemSpacing.y);
  ImGui::PushStyleColor(ImGuiCol_ChildBg,
                        on_snapshot ? theme::mix(theme::palette::surface,
                                                 theme::palette::warning, 0.30f)
                                    : theme::palette::surface);
  // A borderless child ignores WindowPadding unless asked. Y stays 0, the row
  // centres itself.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(theme::metrics::window_pad_x, 0.0f));
  ImGui::BeginChild("##version_strip", ImVec2(0, 0),
                    ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  {
    float row_h = ImGui::GetFrameHeight();
    float avail_h = ImGui::GetContentRegionAvail().y;
    if (avail_h > row_h)
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail_h - row_h) * 0.5f);
  }
  auto download_button = [&](bool same_line) {
    if (!top_bar.on_download_map)
      return;
    if (same_line) {
      ImGui::SameLine();
      ImGui::TextDisabled("|");
      ImGui::SameLine();
    }
    if (ImGui::Button(ICON_MDI_DOWNLOAD))
      top_bar.on_download_map();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Download map");
  };

  ImGui::AlignTextToFramePadding();
  if (!top_bar.has_server) {
    // Nothing else in this strip means anything without a server.
    download_button(/*same_line=*/false);
    if (!state.status_message.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", state.status_message.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return;
  }
  if (!state.branch.empty()) {
    ImGui::TextUnformatted("Branch:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("##branch_combo", state.branch.c_str())) {
      for (const auto &b : state.branches) {
        bool sel = (b == state.branch);
        if (ImGui::Selectable(b.c_str(), sel) && b != state.branch)
          state.branch_switch_to = b;
      }
      ImGui::Separator();
      if (ImGui::SmallButton("Refresh##branchlist"))
        state.branch_request_refresh = true;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
  }
  ImGui::AlignTextToFramePadding();
  if (on_snapshot)
    ImGui::TextColored(theme::palette::warning, "Read-only snapshot:");
  else
    ImGui::TextUnformatted("Version:");
  ImGui::SameLine();

  std::string preview =
      on_snapshot ? state.snapshot_dir : std::string("latest");
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
      std::snprintf(label, sizeof(label), "%s  %04d-%02d-%02d %02d:%02d",
                    s.sha.c_str(), tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                    tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min);
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
    ImGui::PushStyleColor(ImGuiCol_Button,
                          theme::with_alpha(theme::palette::danger, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::palette::danger);
    if (ImGui::Button("Restore to latest"))
      ImGui::OpenPopup("Restore snapshot##restore_confirm");
    ImGui::PopStyleColor(2);
  } else {
    if (ImGui::Button("Snapshot"))
      state.snapshot_request_create = true;
  }

  if (ImGuiWidgets::BeginModal("Restore snapshot##restore_confirm", 380.0f)) {
    ImGui::TextWrapped("Overwrite latest with snapshot %s?",
                       state.snapshot_dir.c_str());
    ImGui::TextDisabled("Connected users will see the restored state.");
    const int a = ImGuiWidgets::ModalActions("Restore", "Cancel");
    if (a == 1) {
      state.snapshot_request_restore = state.snapshot_dir;
      ImGui::CloseCurrentPopup();
    } else if (a == 2) {
      ImGui::CloseCurrentPopup();
    }
    ImGuiWidgets::EndModal();
  }

  if (!state.snapshot_status.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.snapshot_status.c_str());
  }

  if (on_snapshot) {
    ImGui::SameLine();
    if (ImGui::Button("Deploy to..."))
      ImGui::OpenPopup("##deploy_popup");
    if (ImGui::BeginPopup("##deploy_popup")) {
      ImGui::TextDisabled("Copy snapshot into branch:");
      ImGui::Separator();
      bool any = false;
      for (const auto &b : state.branches) {
        if (b == state.branch)
          continue;
        any = true;
        if (ImGui::Selectable(b.c_str())) {
          state.deploy_request_dir = state.snapshot_dir;
          state.deploy_request_to = b;
          ImGui::CloseCurrentPopup();
        }
      }
      if (!any)
        ImGui::TextDisabled("(no other branches)");
      ImGui::Separator();
      if (ImGui::SmallButton("Refresh##branches"))
        state.branch_request_refresh = true;
      ImGui::EndPopup();
    }
  } else {
    ImGui::SameLine();
    if (ImGui::Button("Deploy latest to..."))
      ImGui::OpenPopup("##deploy_latest_popup");
    if (ImGui::BeginPopup("##deploy_latest_popup")) {
      ImGui::TextDisabled("Copy latest map into branch:");
      ImGui::Separator();
      bool any = false;
      for (const auto &b : state.branches) {
        if (b == state.branch)
          continue;
        any = true;
        if (ImGui::Selectable(b.c_str())) {
          state.deploy_latest_to = b;
          ImGui::CloseCurrentPopup();
        }
      }
      if (!any)
        ImGui::TextDisabled("(no other branches)");
      ImGui::Separator();
      if (ImGui::SmallButton("Refresh##branches_latest"))
        state.branch_request_refresh = true;
      ImGui::EndPopup();
    }
  }

  if (!state.deploy_status.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.deploy_status.c_str());
  }
  download_button(/*same_line=*/true);
  // draw_status_bar is browser-only, so this is the desktop's only feedback.
  if (!state.status_message.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", state.status_message.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void set_yjs_readonly(bool v) { g_readonly = v; }
bool yjs_readonly() { return g_readonly; }

} // namespace imrmf::map_editor
