// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#define IMGUI_DEFINE_MATH_OPERATORS

#include "GLFW/glfw3.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/imgui.h"

#include "model/building.hpp"
#include "model/yaml_io.hpp"
#include "view/editor_view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

using imrmf::map_editor::Building;
using imrmf::map_editor::EditorState;
using imrmf::map_editor::EditorView;

namespace {

GLFWwindow *g_window = nullptr;
Building g_building;
EditorState g_state;
std::unique_ptr<EditorView> g_view;
std::string g_building_id;
std::string g_server_url;

enum class ConnPhase {
  BootingConfig, // GET /config in flight (decides whether to show modal)
  Modal,         // showing the connection dialog
  Mounting,      // POST /mount in flight (or GET /buildings in locked mode)
  Mounted,       // mount succeeded, user picks a building
  Loading,       // POST /buildings/:id/load in flight
  Connecting,    // Yjs WebSocketProvider syncing
  Connected,     // editor is live
  Error,         // last operation failed
};

struct ConnectionForm {
  int kind_idx = 0; // 0 = Local, 1 = S3
  char server_url[256] = "";
  char local_path[512] = "";
  char s3_bucket[128] = "";
  char s3_prefix[128] = "";
  char s3_region[64] = "us-east-1";
  char s3_access[128] = "";
  char s3_secret[256] = "";
  char s3_endpoint[256] = "";
  char building_id[64] = "";
};

ConnPhase g_phase = ConnPhase::BootingConfig;
ConnectionForm g_form;
std::vector<std::string> g_buildings;
std::string g_error_message;

bool g_locked = false;
std::string g_auto_building;

struct FsEntry {
  std::string name;
  bool is_dir = false;
  bool is_building_yaml = false;
};

struct FsBrowser {
  std::string current_path;
  std::string parent_path;
  std::string root;
  std::vector<FsEntry> entries;
  bool loading = false;
  bool requested_once = false;
  std::string error;
};
FsBrowser g_fs;

#ifdef __EMSCRIPTEN__
// clang-format off

// Yjs bridge. Each call is a no-op until `window.imrmf.connect(...)` succeeds.
EM_JS(int, imrmf_yjs_remote_dirty, (), {
  return (window.imrmf && window.imrmf.yjs && window.imrmf.yjs.isRemoteDirty())
             ? 1
             : 0;
});
EM_JS(void, imrmf_yjs_clear_remote_dirty, (), {
  if (window.imrmf && window.imrmf.yjs)
    window.imrmf.yjs.clearRemoteDirty();
});
EM_JS(const char *, imrmf_yjs_snapshot_yaml, (), {
  if (!window.imrmf || !window.imrmf.yjs)
    return stringToNewUTF8("");
  try {
    return stringToNewUTF8(window.imrmf.yjs.snapshotYaml());
  } catch (e) {
    console.error('[imrmf] snapshot failed:', e);
    return stringToNewUTF8("");
  }
});
EM_JS(void, imrmf_yjs_push_local_yaml, (const char *yaml_c), {
  if (!window.imrmf || !window.imrmf.yjs)
    return;
  try {
    window.imrmf.yjs.applyLocalYaml(UTF8ToString(yaml_c));
  } catch (e) {
    console.error('[imrmf] push failed:', e);
  }
});
EM_JS(int, imrmf_yjs_is_synced, (), {
  return (window.imrmf && window.imrmf.yjs && window.imrmf.yjs.isSynced()) ? 1
                                                                           : 0;
});

// Async control plane. C++ pokes these and watches a status code via
// imrmf_status() rather than awaiting promises.
//   0 = idle
//   1 = in flight
//   2 = success (most recent operation completed)
//   3 = error
EM_JS(void, imrmf_session_clear, (), {
  if (window.imrmf)
    window.imrmf.clearSession();
});
EM_JS(const char *, imrmf_session_get, (), {
  if (!window.imrmf)
    return stringToNewUTF8("");
  const s = window.imrmf.getSession();
  return stringToNewUTF8(s ? JSON.stringify(s) : "");
});
EM_JS(void, imrmf_session_save, (const char *json_c), {
  if (!window.imrmf)
    return;
  try {
    window.imrmf.saveSession(JSON.parse(UTF8ToString(json_c)));
  } catch (_) {
  }
});

// Pending result, polled by C++. Set to a result code + optional payload.
//   _result.code in {idle, busy, ok, err}
//   _result.payload = arbitrary JSON for the latest operation
EM_JS(void, imrmf_init_state, (), {
  if (!window.imrmf)
    return;
  window.imrmf._result = {code : 'idle', payload : null};
});
EM_JS(const char *, imrmf_result_code, (), {
  if (!window.imrmf || !window.imrmf._result)
    return stringToNewUTF8("idle");
  return stringToNewUTF8(window.imrmf._result.code || "idle");
});
EM_JS(const char *, imrmf_result_payload, (), {
  if (!window.imrmf || !window.imrmf._result)
    return stringToNewUTF8("");
  const p = window.imrmf._result.payload;
  return stringToNewUTF8(
      p == null ? "" : (typeof p === 'string' ? p : JSON.stringify(p)));
});
EM_JS(void, imrmf_reset_result, (), {
  if (window.imrmf && window.imrmf._result) {
    window.imrmf._result = {code : 'idle', payload : null};
  }
});

EM_JS(void, imrmf_call_mount, (const char *server_c, const char *cfg_json_c), {
  if (!window.imrmf)
    return;
  window.imrmf._result = {code : 'busy', payload : null};
  const server = UTF8ToString(server_c);
  const cfg = JSON.parse(UTF8ToString(cfg_json_c));
  window.imrmf.mount(server, cfg)
      .then(r => { window.imrmf._result = {code : 'ok', payload : r}; })
      .catch(e => {
        window.imrmf._result = {code : 'err', payload : String(e)};
      });
});

EM_JS(void, imrmf_call_fetch_config, (const char *server_c), {
  if (!window.imrmf)
    return;
  window.imrmf._result = {code : 'busy', payload : null};
  window.imrmf.fetchConfig(UTF8ToString(server_c))
      .then(r => { window.imrmf._result = {code : 'ok', payload : r}; })
      .catch(e => {
        window.imrmf._result = {code : 'err', payload : String(e)};
      });
});

EM_JS(void, imrmf_call_list_buildings, (const char *server_c), {
  if (!window.imrmf)
    return;
  window.imrmf._result = {code : 'busy', payload : null};
  window.imrmf.listBuildings(UTF8ToString(server_c))
      .then(r => { window.imrmf._result = {code : 'ok', payload : r}; })
      .catch(e => {
        window.imrmf._result = {code : 'err', payload : String(e)};
      });
});

EM_JS(void, imrmf_call_load_building,
      (const char *server_c, const char *building_c), {
        if (!window.imrmf)
          return;
        window.imrmf._result = {code : 'busy', payload : null};
        window.imrmf
            .loadBuilding(UTF8ToString(server_c), UTF8ToString(building_c))
            .then(() => { window.imrmf._result = {code : 'ok', payload : null}; })
            .catch(e => {
              window.imrmf._result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_connect, (const char *server_c, const char *building_c),
      {
        if (!window.imrmf)
          return;
        window.imrmf._result = {code : 'busy', payload : null};
        window.imrmf.connect(UTF8ToString(server_c), UTF8ToString(building_c))
            .then(() => { window.imrmf._result = {code : 'ok', payload : null}; })
            .catch(e => {
              window.imrmf._result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_disconnect, (const char *server_c), {
  if (!window.imrmf)
    return;
  window.imrmf.disconnect();
  if (server_c) {
    window.imrmf.unmount(UTF8ToString(server_c)).catch(() => {});
  }
});

EM_JS(const char *, imrmf_default_server_url, (),
      { return stringToNewUTF8(window.location.origin || ""); });

// Filesystem browser. Separate result slot from the mount-flow so polling
// one doesn't interfere with the other.
EM_JS(void, imrmf_call_fs_list, (const char *server_c, const char *path_c), {
  if (!window.imrmf)
    return;
  window.imrmf._fs_result = {code : 'busy', payload : null};
  let base = UTF8ToString(server_c) || window.location.origin;
  while (base.length > 0 && base[base.length - 1] === '/') {
    base = base.substring(0, base.length - 1);
  }
  const path = UTF8ToString(path_c);
  const url =
      base + "/fs/list" + (path ? ("?path=" + encodeURIComponent(path)) : "");
  fetch(url)
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(d => { window.imrmf._fs_result = {code : 'ok', payload : d}; })
      .catch(e => {
        window.imrmf._fs_result = {code : 'err', payload : String(e)};
      });
});
EM_JS(const char *, imrmf_fs_result_code, (), {
  if (!window.imrmf || !window.imrmf._fs_result)
    return stringToNewUTF8('idle');
  return stringToNewUTF8(window.imrmf._fs_result.code || 'idle');
});
EM_JS(const char *, imrmf_fs_result_payload, (), {
  if (!window.imrmf || !window.imrmf._fs_result)
    return stringToNewUTF8("");
  const p = window.imrmf._fs_result.payload;
  return stringToNewUTF8(
      p == null ? "" : (typeof p === 'string' ? p : JSON.stringify(p)));
});
EM_JS(void, imrmf_fs_reset_result, (), {
  if (window.imrmf)
    window.imrmf._fs_result = {code : 'idle', payload : null};
});

// clang-format on
#else // !__EMSCRIPTEN__

int imrmf_yjs_remote_dirty() { return 0; }
void imrmf_yjs_clear_remote_dirty() {}
const char *imrmf_yjs_snapshot_yaml() { return nullptr; }
void imrmf_yjs_push_local_yaml(const char *) {}
int imrmf_yjs_is_synced() { return 0; }

#endif

void mirror_from_yjs() {
#ifdef __EMSCRIPTEN__
  if (!imrmf_yjs_remote_dirty())
    return;
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
      ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
      ImGui::GetIO().WantTextInput) {
    return;
  }
  const char *yaml = imrmf_yjs_snapshot_yaml();
  if (!yaml)
    return;
  std::string body(yaml);
  std::free((void *)yaml);
  if (body.empty()) {
    imrmf_yjs_clear_remote_dirty();
    return;
  }
  try {
    auto b = imrmf::map_editor::parse_building(body);
    if (!b.levels.empty()) {
      g_building = std::move(b);
      g_state.level_idx = std::max(
          0, std::min(g_state.level_idx, (int)g_building.levels.size() - 1));
      const auto &cur = g_building.levels[g_state.level_idx];
      g_state.selected_vertices.erase(
          std::remove_if(
              g_state.selected_vertices.begin(),
              g_state.selected_vertices.end(),
              [&](int i) { return i < 0 || i >= (int)cur.vertices.size(); }),
          g_state.selected_vertices.end());
      g_state.selected_lanes.erase(
          std::remove_if(
              g_state.selected_lanes.begin(), g_state.selected_lanes.end(),
              [&](int i) { return i < 0 || i >= (int)cur.lanes.size(); }),
          g_state.selected_lanes.end());
      if (g_state.selected_layer >= (int)cur.layers.size())
        g_state.selected_layer = -1;
      if (g_state.pending_lane_start >= (int)cur.vertices.size())
        g_state.pending_lane_start = -1;
      g_state.dirty = false;
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[imrmf] yjs mirror parse failed: %s\n", e.what());
  }
  imrmf_yjs_clear_remote_dirty();
#endif
}

void push_to_yjs_if_dirty() {
#ifdef __EMSCRIPTEN__
  if (!g_state.dirty)
    return;
  static double s_last_push = 0.0;
  const double now = emscripten_get_now() * 0.001;
  const bool any_mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                              ImGui::IsMouseDown(ImGuiMouseButton_Right);
  if (any_mouse_down && (now - s_last_push) < 0.2)
    return;
  std::string yaml = imrmf::map_editor::serialize_building(g_building);
  imrmf_yjs_push_local_yaml(yaml.c_str());
  s_last_push = now;
  g_state.dirty = false;
#endif
}

#ifdef __EMSCRIPTEN__
std::string take_string(const char *c) {
  if (!c)
    return {};
  std::string s(c);
  std::free((void *)c);
  return s;
}

std::string build_mount_json(const ConnectionForm &f) {
  auto esc = [](const char *s) {
    std::string out;
    for (const char *p = s; *p; ++p) {
      if (*p == '"' || *p == '\\')
        out.push_back('\\');
      out.push_back(*p);
    }
    return out;
  };
  if (f.kind_idx == 0) {
    return std::string("{\"kind\":\"local\",\"path\":\"") + esc(f.local_path) +
           "\"}";
  }
  std::string j = "{\"kind\":\"s3\"";
  j += ",\"bucket\":\"" + esc(f.s3_bucket) + "\"";
  j += ",\"prefix\":\"" + esc(f.s3_prefix) + "\"";
  j += ",\"region\":\"" + esc(f.s3_region) + "\"";
  j += ",\"access_key_id\":\"" + esc(f.s3_access) + "\"";
  j += ",\"secret_access_key\":\"" + esc(f.s3_secret) + "\"";
  if (f.s3_endpoint[0])
    j += ",\"endpoint_url\":\"" + esc(f.s3_endpoint) + "\"";
  j += "}";
  return j;
}

void start_mount() {
  g_phase = ConnPhase::Mounting;
  g_server_url = g_form.server_url;
  if (g_server_url.empty())
    g_server_url = take_string(imrmf_default_server_url());
  std::string cfg = build_mount_json(g_form);
  std::strncpy(g_form.server_url, g_server_url.c_str(),
               sizeof(g_form.server_url) - 1);
  g_form.server_url[sizeof(g_form.server_url) - 1] = '\0';
  imrmf_reset_result();
  imrmf_call_mount(g_server_url.c_str(), cfg.c_str());
}

void start_boot_config() {
  g_phase = ConnPhase::BootingConfig;
  if (g_server_url.empty())
    g_server_url = take_string(imrmf_default_server_url());
  imrmf_reset_result();
  imrmf_call_fetch_config(g_server_url.c_str());
}

void start_list_buildings() {
  g_phase = ConnPhase::Mounting;
  if (g_server_url.empty())
    g_server_url = take_string(imrmf_default_server_url());
  imrmf_reset_result();
  imrmf_call_list_buildings(g_server_url.c_str());
}

void parse_buildings_payload(const std::string &payload) {
  g_buildings.clear();
  // Payload is `{"buildings":[...]}`. Minimal parse since we control the
  // server format. Anything funky surfaces in the UI as "no buildings".
  auto open = payload.find('[');
  auto close = payload.find(']', open);
  if (open == std::string::npos || close == std::string::npos)
    return;
  std::string body = payload.substr(open + 1, close - open - 1);
  size_t pos = 0;
  while (pos < body.size()) {
    auto q1 = body.find('"', pos);
    if (q1 == std::string::npos)
      break;
    auto q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos)
      break;
    g_buildings.push_back(body.substr(q1 + 1, q2 - q1 - 1));
    pos = q2 + 1;
  }
}

void start_load_building() {
  g_phase = ConnPhase::Loading;
  imrmf_reset_result();
  imrmf_call_load_building(g_server_url.c_str(), g_building_id.c_str());
}

void start_connect_yjs() {
  g_phase = ConnPhase::Connecting;
  g_view = std::make_unique<EditorView>(g_server_url, g_building_id);
  imrmf_reset_result();
  imrmf_call_connect(g_server_url.c_str(), g_building_id.c_str());
}

void save_session() {
  std::string cfg = build_mount_json(g_form);
  std::string esc;
  for (char c : cfg) {
    if (c == '"' || c == '\\')
      esc.push_back('\\');
    esc.push_back(c);
  }
  std::string esc_server;
  for (char c : g_server_url) {
    if (c == '"' || c == '\\')
      esc_server.push_back('\\');
    esc_server.push_back(c);
  }
  std::string esc_id;
  for (char c : g_building_id) {
    if (c == '"' || c == '\\')
      esc_id.push_back('\\');
    esc_id.push_back(c);
  }
  std::string json = "{\"server_url\":\"" + esc_server +
                     "\",\"building_id\":\"" + esc_id + "\",\"mount\":" + cfg +
                     "}";
  imrmf_session_save(json.c_str());
}

void try_restore_session() {
  std::string raw = take_string(imrmf_session_get());
  if (raw.empty())
    return;
  auto find_string = [&](const char *key) -> std::string {
    std::string needle = std::string("\"") + key + "\":\"";
    auto pos = raw.find(needle);
    if (pos == std::string::npos)
      return {};
    pos += needle.size();
    std::string out;
    while (pos < raw.size() && raw[pos] != '"') {
      if (raw[pos] == '\\' && pos + 1 < raw.size()) {
        out.push_back(raw[pos + 1]);
        pos += 2;
      } else {
        out.push_back(raw[pos++]);
      }
    }
    return out;
  };
  std::string server = find_string("server_url");
  std::string bid = find_string("building_id");
  if (server.empty() || bid.empty())
    return;
  std::strncpy(g_form.server_url, server.c_str(),
               sizeof(g_form.server_url) - 1);
  std::strncpy(g_form.building_id, bid.c_str(), sizeof(g_form.building_id) - 1);
  g_form.server_url[sizeof(g_form.server_url) - 1] = '\0';
  g_form.building_id[sizeof(g_form.building_id) - 1] = '\0';
  g_server_url = server;
  g_building_id = bid;
  // Detect kind from the mount blob.
  auto kind_pos = raw.find("\"kind\":\"");
  if (kind_pos != std::string::npos) {
    std::string kind = find_string("kind");
    g_form.kind_idx = (kind == "s3") ? 1 : 0;
    if (g_form.kind_idx == 0) {
      std::strncpy(g_form.local_path, find_string("path").c_str(),
                   sizeof(g_form.local_path) - 1);
      g_form.local_path[sizeof(g_form.local_path) - 1] = '\0';
    } else {
      std::strncpy(g_form.s3_bucket, find_string("bucket").c_str(),
                   sizeof(g_form.s3_bucket) - 1);
      std::strncpy(g_form.s3_prefix, find_string("prefix").c_str(),
                   sizeof(g_form.s3_prefix) - 1);
      std::strncpy(g_form.s3_region, find_string("region").c_str(),
                   sizeof(g_form.s3_region) - 1);
      std::strncpy(g_form.s3_access, find_string("access_key_id").c_str(),
                   sizeof(g_form.s3_access) - 1);
      std::strncpy(g_form.s3_secret, find_string("secret_access_key").c_str(),
                   sizeof(g_form.s3_secret) - 1);
      std::strncpy(g_form.s3_endpoint, find_string("endpoint_url").c_str(),
                   sizeof(g_form.s3_endpoint) - 1);
    }
  }
  start_mount();
}

void start_fs_list(const std::string &path) {
  g_fs.loading = true;
  g_fs.error.clear();
  imrmf_fs_reset_result();
  std::string server =
      g_form.server_url[0] ? std::string(g_form.server_url) : std::string();
  imrmf_call_fs_list(server.c_str(), path.c_str());
}

// Minimal JSON string extractor: pulls "<key>":"<value>" with naive escape
// handling. Good enough for the well-known /fs/list response we control.
std::string json_string_field(const std::string &src, const std::string &key) {
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

bool json_bool_field(const std::string &src, const std::string &key) {
  std::string needle = "\"" + key + "\":";
  auto pos = src.find(needle);
  if (pos == std::string::npos)
    return false;
  pos += needle.size();
  while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t'))
    ++pos;
  return src.compare(pos, 4, "true") == 0;
}

void parse_fs_list_payload(const std::string &payload) {
  g_fs.entries.clear();
  g_fs.root = json_string_field(payload, "root");
  g_fs.current_path = json_string_field(payload, "path");
  // parent may be null
  std::string parent_marker = "\"parent\":";
  auto pp = payload.find(parent_marker);
  if (pp != std::string::npos) {
    auto val_start = pp + parent_marker.size();
    while (val_start < payload.size() &&
           (payload[val_start] == ' ' || payload[val_start] == '\t'))
      ++val_start;
    if (payload.compare(val_start, 4, "null") == 0) {
      g_fs.parent_path.clear();
    } else {
      g_fs.parent_path = json_string_field(payload, "parent");
    }
  }
  // entries is an array of objects.
  std::string en_marker = "\"entries\":[";
  auto ep = payload.find(en_marker);
  if (ep == std::string::npos)
    return;
  ep += en_marker.size();
  while (ep < payload.size()) {
    auto obj_start = payload.find('{', ep);
    if (obj_start == std::string::npos)
      break;
    auto obj_end = payload.find('}', obj_start);
    if (obj_end == std::string::npos)
      break;
    std::string obj = payload.substr(obj_start, obj_end - obj_start + 1);
    FsEntry e;
    e.name = json_string_field(obj, "name");
    e.is_dir = json_bool_field(obj, "is_dir");
    e.is_building_yaml = json_bool_field(obj, "is_building_yaml");
    if (!e.name.empty())
      g_fs.entries.push_back(e);
    ep = obj_end + 1;
    // stop at the array's closing bracket
    auto next_brace = payload.find('{', ep);
    auto close_bracket = payload.find(']', ep);
    if (close_bracket != std::string::npos &&
        (next_brace == std::string::npos || close_bracket < next_brace)) {
      break;
    }
  }
}

void poll_fs_result() {
  if (!g_fs.loading)
    return;
  std::string code = take_string(imrmf_fs_result_code());
  if (code == "busy" || code == "idle")
    return;
  std::string payload = take_string(imrmf_fs_result_payload());
  if (code == "err") {
    g_fs.error = payload.empty() ? "fs list failed" : payload;
    g_fs.loading = false;
    imrmf_fs_reset_result();
    return;
  }
  parse_fs_list_payload(payload);
  g_fs.loading = false;
  imrmf_fs_reset_result();
}

void poll_async_result() {
  if (g_phase != ConnPhase::BootingConfig && g_phase != ConnPhase::Mounting &&
      g_phase != ConnPhase::Loading && g_phase != ConnPhase::Connecting)
    return;
  std::string code = take_string(imrmf_result_code());
  if (code == "busy" || code == "idle")
    return;
  std::string payload = take_string(imrmf_result_payload());
  if (code == "err") {
    if (g_phase == ConnPhase::BootingConfig) {
      // /config unreachable (old server) — fall through to legacy flow.
      g_phase = ConnPhase::Modal;
      try_restore_session();
      imrmf_reset_result();
      return;
    }
    g_error_message = payload.empty() ? "unknown error" : payload;
    g_phase = ConnPhase::Error;
    imrmf_reset_result();
    return;
  }
  if (g_phase == ConnPhase::BootingConfig) {
    g_locked = json_bool_field(payload, "locked");
    if (g_locked) {
      g_auto_building = json_string_field(payload, "auto_building");
      start_list_buildings();
    } else {
      g_phase = ConnPhase::Modal;
      try_restore_session();
    }
  } else if (g_phase == ConnPhase::Mounting) {
    parse_buildings_payload(payload);
    if (g_buildings.empty()) {
      g_error_message = g_locked
                            ? "server has no buildings to serve"
                            : "mount succeeded but no buildings were found";
      g_phase = ConnPhase::Error;
    } else {
      g_phase = ConnPhase::Mounted;
      if (g_locked && !g_auto_building.empty() &&
          std::find(g_buildings.begin(), g_buildings.end(), g_auto_building) !=
              g_buildings.end()) {
        g_building_id = g_auto_building;
        start_load_building();
      } else {
        if (g_building_id.empty() ||
            std::find(g_buildings.begin(), g_buildings.end(), g_building_id) ==
                g_buildings.end()) {
          g_building_id = g_buildings.front();
          std::strncpy(g_form.building_id, g_building_id.c_str(),
                       sizeof(g_form.building_id) - 1);
          g_form.building_id[sizeof(g_form.building_id) - 1] = '\0';
        }
        if (g_locked && g_buildings.size() == 1) {
          start_load_building();
        } else if (!g_locked && g_form.building_id[0] &&
                   g_building_id == g_form.building_id) {
          start_load_building();
        }
      }
    }
  } else if (g_phase == ConnPhase::Loading) {
    start_connect_yjs();
  } else if (g_phase == ConnPhase::Connecting) {
    g_phase = ConnPhase::Connected;
    if (!g_locked)
      save_session();
  }
  imrmf_reset_result();
}

void disconnect_and_reset() {
  imrmf_session_clear();
  imrmf_call_disconnect(g_server_url.c_str());
  g_phase = ConnPhase::Modal;
  g_view.reset();
  g_building = {};
  g_state = {};
  g_building_id.clear();
  g_buildings.clear();
  g_error_message.clear();
}

#endif // __EMSCRIPTEN__

void draw_connection_modal() {
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoCollapse;
  ImGui::Begin("Connect to backend##imrmf", nullptr, flags);

  ImGui::TextWrapped("Choose where your building.yaml lives. The server will "
                     "mount this backend and serve the editor against it.");
  ImGui::Spacing();

  ImGui::InputText("Server URL", g_form.server_url, sizeof(g_form.server_url));
  ImGui::SameLine();
  if (ImGui::SmallButton("origin")) {
#ifdef __EMSCRIPTEN__
    std::string origin = take_string(imrmf_default_server_url());
    std::strncpy(g_form.server_url, origin.c_str(),
                 sizeof(g_form.server_url) - 1);
    g_form.server_url[sizeof(g_form.server_url) - 1] = '\0';
#endif
  }
  ImGui::Spacing();

  const char *kinds[] = {"Local filesystem", "S3"};
  ImGui::Combo("Storage", &g_form.kind_idx, kinds, IM_ARRAYSIZE(kinds));
  ImGui::Spacing();

  if (g_form.kind_idx == 0) {
#ifdef __EMSCRIPTEN__
    // On first render in Local mode, kick off a listing of the server's
    // browse root. The user can then navigate from there.
    if (!g_fs.requested_once) {
      g_fs.requested_once = true;
      start_fs_list("");
    }
#endif
    ImGui::InputText("Path", g_form.local_path, sizeof(g_form.local_path));
    ImGui::TextDisabled("Pick a directory below or paste an absolute path.");

    ImGui::Spacing();
    if (g_fs.loading) {
      ImGui::TextDisabled("loading...");
    } else if (!g_fs.error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                         g_fs.error.c_str());
    }

    ImGui::Text("Current:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", g_fs.current_path.empty()
                                 ? "(not loaded)"
                                 : g_fs.current_path.c_str());

    if (!g_fs.parent_path.empty()) {
      if (ImGui::SmallButton("..")) {
#ifdef __EMSCRIPTEN__
        start_fs_list(g_fs.parent_path);
#endif
      }
      ImGui::SameLine();
    }
    if (ImGui::SmallButton("refresh")) {
#ifdef __EMSCRIPTEN__
      start_fs_list(g_fs.current_path);
#endif
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("use current")) {
      std::strncpy(g_form.local_path, g_fs.current_path.c_str(),
                   sizeof(g_form.local_path) - 1);
      g_form.local_path[sizeof(g_form.local_path) - 1] = '\0';
    }

    ImGui::Spacing();
    ImGui::BeginChild("##fs_entries", ImVec2(0, 220), true);
    for (const auto &e : g_fs.entries) {
      if (e.is_dir) {
        std::string label = "[d] " + e.name;
        if (ImGui::Selectable(label.c_str(), false)) {
#ifdef __EMSCRIPTEN__
          std::string next = g_fs.current_path;
          if (!next.empty() && next.back() != '/')
            next.push_back('/');
          next += e.name;
          start_fs_list(next);
#endif
        }
      } else {
        ImVec4 col = e.is_building_yaml ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                        : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        ImGui::TextColored(col, "    %s%s", e.name.c_str(),
                           e.is_building_yaml ? " (building.yaml)" : "");
      }
    }
    ImGui::EndChild();
  } else {
    ImGui::InputText("Bucket", g_form.s3_bucket, sizeof(g_form.s3_bucket));
    ImGui::InputText("Prefix", g_form.s3_prefix, sizeof(g_form.s3_prefix));
    ImGui::InputText("Region", g_form.s3_region, sizeof(g_form.s3_region));
    ImGui::InputText("Access key id", g_form.s3_access,
                     sizeof(g_form.s3_access));
    ImGui::InputText("Secret access key", g_form.s3_secret,
                     sizeof(g_form.s3_secret), ImGuiInputTextFlags_Password);
    ImGui::InputText("Endpoint url (optional)", g_form.s3_endpoint,
                     sizeof(g_form.s3_endpoint));
    ImGui::TextDisabled(
        "Leave endpoint url empty for AWS. Set it for minio etc.");
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  bool can_connect =
      g_form.server_url[0] != '\0' ||
      true; // empty server url falls back to window.location.origin
  if (g_form.kind_idx == 0)
    can_connect = can_connect && g_form.local_path[0] != '\0';
  else {
    can_connect = can_connect && g_form.s3_bucket[0] && g_form.s3_region[0] &&
                  g_form.s3_access[0] && g_form.s3_secret[0];
  }

  if (!can_connect)
    ImGui::BeginDisabled();
  if (ImGui::Button("Connect", ImVec2(120, 0))) {
#ifdef __EMSCRIPTEN__
    start_mount();
#endif
  }
  if (!can_connect)
    ImGui::EndDisabled();

  if (!g_error_message.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                       g_error_message.c_str());
  }
  ImGui::End();
}

void draw_building_picker() {
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(420, 0));
  ImGui::Begin("Choose building##imrmf", nullptr,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse);
  int selected = -1;
  for (int i = 0; i < (int)g_buildings.size(); ++i) {
    if (g_buildings[i] == g_building_id)
      selected = i;
  }
  if (ImGui::BeginCombo("Building",
                        selected >= 0 ? g_buildings[selected].c_str() : "")) {
    for (int i = 0; i < (int)g_buildings.size(); ++i) {
      bool is_sel = (i == selected);
      if (ImGui::Selectable(g_buildings[i].c_str(), is_sel)) {
        g_building_id = g_buildings[i];
        std::strncpy(g_form.building_id, g_building_id.c_str(),
                     sizeof(g_form.building_id) - 1);
        g_form.building_id[sizeof(g_form.building_id) - 1] = '\0';
      }
      if (is_sel)
        ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  ImGui::Spacing();
  if (ImGui::Button("Open", ImVec2(120, 0))) {
#ifdef __EMSCRIPTEN__
    if (!g_building_id.empty())
      start_load_building();
#endif
  }
  if (!g_locked) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
#ifdef __EMSCRIPTEN__
      disconnect_and_reset();
#endif
    }
  }
  ImGui::End();
}

void draw_busy(const char *label) {
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::Begin("##imrmf_busy", nullptr,
               ImGuiWindowFlags_NoDecoration |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
  ImGui::Text("%s", label);
  ImGui::End();
}

imrmf::map_editor::TopBarHooks build_top_bar_hooks() {
  imrmf::map_editor::TopBarHooks h;
  std::string summary;
  if (g_form.kind_idx == 0) {
    summary = "Local \xC2\xB7 ";
    summary += g_form.local_path;
  } else {
    summary = "S3 \xC2\xB7 ";
    summary += g_form.s3_bucket;
    if (g_form.s3_prefix[0]) {
      summary += "/";
      summary += g_form.s3_prefix;
    }
  }
  if (!g_building_id.empty()) {
    summary += " \xC2\xB7 ";
    summary += g_building_id;
  }
  h.connection_label = std::move(summary);
  h.can_disconnect = !g_locked;
  h.on_disconnect = []() {
#ifdef __EMSCRIPTEN__
    disconnect_and_reset();
#endif
  };
  return h;
}

void frame() {
  glfwPollEvents();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

#ifdef __EMSCRIPTEN__
  poll_async_result();
  poll_fs_result();
#endif

  if (g_phase == ConnPhase::Connected) {
    mirror_from_yjs();

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("##imrmf_root", nullptr, wf);
    ImGui::PopStyleVar();
    if (g_view && !g_building.levels.empty()) {
      g_view->draw(g_building, g_state, []() {}, build_top_bar_hooks());
    } else {
      ImGui::Text("Waiting for initial sync...");
    }
    ImGui::End();
    push_to_yjs_if_dirty();
  } else if (g_phase == ConnPhase::Modal || g_phase == ConnPhase::Error) {
    draw_connection_modal();
  } else if (g_phase == ConnPhase::Mounted) {
    draw_building_picker();
  } else if (g_phase == ConnPhase::BootingConfig) {
    draw_busy("contacting server...");
  } else if (g_phase == ConnPhase::Mounting) {
    draw_busy(g_locked ? "loading server state..." : "mounting backend...");
  } else if (g_phase == ConnPhase::Loading) {
    draw_busy("loading building...");
  } else if (g_phase == ConnPhase::Connecting) {
    draw_busy("connecting...");
  }

  ImGui::Render();
  int w, h;
  glfwGetFramebufferSize(g_window, &w, &h);
  glViewport(0, 0, w, h);
  glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifndef __EMSCRIPTEN__
  glfwSwapBuffers(g_window);
#endif
}

} // namespace

int main() {
  if (!glfwInit()) {
    std::fprintf(stderr, "glfw init failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  g_window = glfwCreateWindow(1600, 900, "ImRmfMapEditor", nullptr, nullptr);
  if (!g_window) {
    std::fprintf(stderr, "glfw window failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(g_window);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  ImGui_ImplGlfw_InitForOpenGL(g_window, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_window, "#canvas");
  imrmf_init_state();
  start_boot_config();
#else
  ImGui_ImplOpenGL3_Init("#version 330");
#endif

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(frame, 0, 1);
#else
  while (!glfwWindowShouldClose(g_window))
    frame();
#endif

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(g_window);
  glfwTerminate();
  return 0;
}
