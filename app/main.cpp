// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

#define IMGUI_DEFINE_MATH_OPERATORS

#include "GLFW/glfw3.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/imgui.h"

#include "ui/font.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#ifndef __EMSCRIPTEN__
#include "app/file_dialog.hpp"
#include "app/window_chrome.hpp"
#include "client_rust/client.h"
#endif

#include "model/building.hpp"
#include "model/yaml_io.hpp"
#include "view/editor_view.hpp"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#else
#include <filesystem>
#include <fstream>
#include <iterator>
#endif

using imrmf::map_editor::Building;
using imrmf::map_editor::EditorState;
using imrmf::map_editor::EditorView;
#ifndef __EMSCRIPTEN__
using imrmf::map_editor::global_cursor_position;
using imrmf::map_editor::has_custom_titlebar;
using imrmf::map_editor::has_native_file_picker;
using imrmf::map_editor::pick_building_yaml;
using imrmf::map_editor::pick_new_building_yaml;
using imrmf::map_editor::use_custom_titlebar;
#endif

namespace {

GLFWwindow *g_window = nullptr;
Building g_building;
Building g_snapshot_building;
EditorState g_state;
std::unique_ptr<EditorView> g_view;
std::string g_building_id;
std::string g_server_url;
std::string g_active_snapshot_dir;
bool g_snapshots_dirty = true;
bool g_branches_dirty = true;

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
// Browser only: a PUT is in flight and should be followed by a load.
bool g_pending_create = false;
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

// PUT a building. Plain fetch, so the JS shell needs no new helper.
EM_JS(void, imrmf_call_put_building,
      (const char *server_c, const char *building_c, const char *yaml_c), {
        if (!window.imrmf)
          return;
        window.imrmf._result = {code : 'busy', payload : null};
        fetch(UTF8ToString(server_c) + '/buildings/' +
                  encodeURIComponent(UTF8ToString(building_c)),
              {
                method : 'PUT',
                headers : {'content-type' : 'application/yaml'},
                body : UTF8ToString(yaml_c),
              })
            .then(r => r.ok ? r.text() : r.text().then(t => {
              throw new Error(t || ('status ' + r.status));
            }))
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

EM_JS(void, imrmf_call_list_snapshots,
      (const char *server_c, const char *id_c), {
        if (!window.imrmf) return;
        window.imrmf._snap_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/snapshots")
            .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
            .then(d => {
              window.imrmf._snap_result = {code : 'list', payload : d};
            })
            .catch(e => {
              window.imrmf._snap_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_create_snapshot,
      (const char *server_c, const char *id_c), {
        if (!window.imrmf) return;
        window.imrmf._snap_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/snapshots",
              {method : "POST"})
            .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
            .then(d => {
              window.imrmf._snap_result = {code : 'created', payload : d};
            })
            .catch(e => {
              window.imrmf._snap_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_restore_snapshot,
      (const char *server_c, const char *id_c, const char *dir_c), {
        if (!window.imrmf) return;
        window.imrmf._snap_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        const dir = UTF8ToString(dir_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/snapshots/" +
              encodeURIComponent(dir) + "/restore",
              {method : "POST"})
            .then(r => r.ok ? r.text() : r.text().then(t => Promise.reject(t)))
            .then(() => {
              window.imrmf._snap_result = {code : 'restored', payload : dir};
            })
            .catch(e => {
              window.imrmf._snap_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_load_snapshot_yaml,
      (const char *server_c, const char *id_c, const char *dir_c), {
        if (!window.imrmf) return;
        window.imrmf._snap_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        const dir = UTF8ToString(dir_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/snapshots/" +
              encodeURIComponent(dir) + "/yaml")
            .then(r => r.ok ? r.text() : r.text().then(t => Promise.reject(t)))
            .then(t => {
              window.imrmf._snap_result =
                  {code : 'yaml', payload : t, payload_dir : dir};
            })
            .catch(e => {
              window.imrmf._snap_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(const char *, imrmf_snap_result_code, (), {
  if (!window.imrmf || !window.imrmf._snap_result)
    return stringToNewUTF8('idle');
  return stringToNewUTF8(window.imrmf._snap_result.code || 'idle');
});
EM_JS(const char *, imrmf_snap_result_payload, (), {
  if (!window.imrmf || !window.imrmf._snap_result)
    return stringToNewUTF8("");
  const p = window.imrmf._snap_result.payload;
  return stringToNewUTF8(
      p == null ? "" : (typeof p === 'string' ? p : JSON.stringify(p)));
});
EM_JS(const char *, imrmf_snap_result_dir, (), {
  if (!window.imrmf || !window.imrmf._snap_result)
    return stringToNewUTF8("");
  return stringToNewUTF8(window.imrmf._snap_result.payload_dir || "");
});
EM_JS(void, imrmf_snap_reset_result, (), {
  if (window.imrmf)
    window.imrmf._snap_result = {code : 'idle', payload : null};
});

EM_JS(void, imrmf_call_list_branches, (const char *server_c), {
  if (!window.imrmf) return;
  window.imrmf._branch_result = {code : 'busy', payload : null};
  let base = UTF8ToString(server_c) || window.location.origin;
  while (base.length > 0 && base[base.length - 1] === '/')
    base = base.substring(0, base.length - 1);
  fetch(base + "/branches")
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(d => { window.imrmf._branch_result = {code : 'list', payload : d}; })
      .catch(e => {
        window.imrmf._branch_result = {code : 'err', payload : String(e)};
      });
});

EM_JS(void, imrmf_call_deploy_snapshot,
      (const char *server_c, const char *id_c, const char *dir_c,
       const char *to_c), {
        if (!window.imrmf) return;
        window.imrmf._branch_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        const dir = UTF8ToString(dir_c);
        const to = UTF8ToString(to_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/snapshots/" +
                  encodeURIComponent(dir) + "/deploy",
              {
                method : "POST",
                headers : {"Content-Type" : "application/json"},
                body : JSON.stringify({to : to})
              })
            .then(r => r.ok ? r.text() : r.text().then(t => Promise.reject(t)))
            .then(t => {
              window.imrmf._branch_result = {code : 'deployed', payload : t};
            })
            .catch(e => {
              window.imrmf._branch_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_deploy_latest,
      (const char *server_c, const char *id_c, const char *to_c), {
        if (!window.imrmf) return;
        window.imrmf._branch_result = {code : 'busy', payload : null};
        let base = UTF8ToString(server_c) || window.location.origin;
        while (base.length > 0 && base[base.length - 1] === '/')
          base = base.substring(0, base.length - 1);
        const id = UTF8ToString(id_c);
        const to = UTF8ToString(to_c);
        fetch(base + "/buildings/" + encodeURIComponent(id) + "/deploy",
              {
                method : "POST",
                headers : {"Content-Type" : "application/json"},
                body : JSON.stringify({to : to})
              })
            .then(r => r.ok ? r.text() : r.text().then(t => Promise.reject(t)))
            .then(t => {
              window.imrmf._branch_result = {code : 'deployed', payload : t};
            })
            .catch(e => {
              window.imrmf._branch_result = {code : 'err', payload : String(e)};
            });
      });

EM_JS(void, imrmf_call_switch_branch, (const char *server_c, const char *to_c), {
  if (!window.imrmf) return;
  window.imrmf._result = {code : 'busy', payload : null};
  let base = UTF8ToString(server_c) || window.location.origin;
  while (base.length > 0 && base[base.length - 1] === '/')
    base = base.substring(0, base.length - 1);
  fetch(base + "/branch", {
    method : "POST",
    headers : {"Content-Type" : "application/json"},
    body : JSON.stringify({to : UTF8ToString(to_c)})
  })
      .then(r => r.ok ? r.json() : r.text().then(t => Promise.reject(t)))
      .then(d => { window.imrmf._result = {code : 'ok', payload : d}; })
      .catch(e => { window.imrmf._result = {code : 'err', payload : String(e)}; });
});

EM_JS(const char *, imrmf_branch_result_code, (), {
  if (!window.imrmf || !window.imrmf._branch_result)
    return stringToNewUTF8('idle');
  return stringToNewUTF8(window.imrmf._branch_result.code || 'idle');
});
EM_JS(const char *, imrmf_branch_result_payload, (), {
  if (!window.imrmf || !window.imrmf._branch_result)
    return stringToNewUTF8("");
  const p = window.imrmf._branch_result.payload;
  return stringToNewUTF8(
      p == null ? "" : (typeof p === 'string' ? p : JSON.stringify(p)));
});
EM_JS(void, imrmf_branch_reset_result, (), {
  if (window.imrmf)
    window.imrmf._branch_result = {code : 'idle', payload : null};
});

// clang-format on
#else // !__EMSCRIPTEN__

// Backed by //client_rust, same y-sync protocol as the browser.
int imrmf_yjs_remote_dirty() { return imrmf_client_remote_dirty(); }
void imrmf_yjs_clear_remote_dirty() { imrmf_client_clear_remote_dirty(); }
const char *imrmf_yjs_snapshot_yaml() { return imrmf_client_snapshot_yaml(); }
void imrmf_yjs_push_local_yaml(const char *yaml) {
  imrmf_client_string_free(imrmf_client_push_yaml(yaml));
}
int imrmf_yjs_is_synced() { return imrmf_client_is_synced(); }

// Snapshots and branches are still browser-only.
void imrmf_call_list_snapshots(const char *, const char *) {}
void imrmf_call_create_snapshot(const char *, const char *) {}
void imrmf_call_load_snapshot_yaml(const char *, const char *, const char *) {}
void imrmf_call_restore_snapshot(const char *, const char *, const char *) {}
const char *imrmf_snap_result_code() { return nullptr; }
const char *imrmf_snap_result_payload() { return nullptr; }
const char *imrmf_snap_result_dir() { return nullptr; }
void imrmf_snap_reset_result() {}
void imrmf_call_list_branches(const char *) {}
void imrmf_call_deploy_snapshot(const char *, const char *, const char *,
                                const char *) {}
void imrmf_call_deploy_latest(const char *, const char *, const char *) {}
void imrmf_call_switch_branch(const char *, const char *) {}
const char *imrmf_branch_result_code() { return nullptr; }
const char *imrmf_branch_result_payload() { return nullptr; }
void imrmf_branch_reset_result() {}

#endif

// JS hands back malloc'd strings, the Rust client hands back its own.
void free_bridge_string(const char *s) {
#ifdef __EMSCRIPTEN__
  std::free((void *)s);
#else
  imrmf_client_string_free((char *)s);
#endif
}

void mirror_from_yjs() {
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
  free_bridge_string(yaml);
  if (body.empty()) {
    imrmf_yjs_clear_remote_dirty();
    return;
  }
  try {
    auto b = imrmf::map_editor::parse_building(body);
    if (!b.levels.empty()) {
      for (auto &lvl : b.levels) {
        for (const auto &old : g_building.levels) {
          if (old.name == lvl.name && old.mpp_snapshot > 0.0) {
            lvl.mpp_snapshot = old.mpp_snapshot;
            break;
          }
        }
      }
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
      auto clamp_sel = [](std::vector<int> &sel, int n) {
        sel.erase(std::remove_if(sel.begin(), sel.end(),
                                 [&](int i) { return i < 0 || i >= n; }),
                  sel.end());
      };
      clamp_sel(g_state.selected_walls, (int)cur.walls.size());
      clamp_sel(g_state.selected_doors, (int)cur.doors.size());
      clamp_sel(g_state.selected_measurements, (int)cur.measurements.size());
      if (g_state.selected_floor >= (int)cur.floors.size())
        g_state.selected_floor = -1;
      if (g_state.selected_layer >= (int)cur.layers.size())
        g_state.selected_layer = -1;
      if (g_state.align_layer_idx >= (int)cur.layers.size())
        g_state.align_layer_idx = -1;
      if (g_state.pending_lane_start >= (int)cur.vertices.size())
        g_state.pending_lane_start = -1;
      if (g_state.pending_edge_start >= (int)cur.vertices.size())
        g_state.pending_edge_start = -1;
      g_state.dirty = false;
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[imrmf] yjs mirror parse failed: %s\n", e.what());
  }
  imrmf_yjs_clear_remote_dirty();
}

void push_to_yjs_if_dirty() {
  if (!g_state.dirty)
    return;
  static double s_last_push = 0.0;
#ifdef __EMSCRIPTEN__
  const double now = emscripten_get_now() * 0.001;
#else
  const double now = glfwGetTime();
#endif
  const bool any_mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                              ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                              ImGui::IsMouseDown(ImGuiMouseButton_Right);
  if (any_mouse_down && (now - s_last_push) < 0.2)
    return;
  std::string yaml = imrmf::map_editor::serialize_building(g_building);
  imrmf_yjs_push_local_yaml(yaml.c_str());
  s_last_push = now;
  g_state.dirty = false;
}

void enter_snapshot_mode(const std::string &dir, const std::string &yaml) {
  try {
    g_snapshot_building = imrmf::map_editor::parse_building(yaml);
  } catch (const std::exception &e) {
    g_state.snapshot_status = std::string("parse failed: ") + e.what();
    return;
  }
  g_state.snapshot_dir = dir;
  g_active_snapshot_dir = dir;
  if (g_view)
    g_view->apply_snapshot_dir(dir);
  imrmf::map_editor::set_yjs_readonly(true);
  g_state.snapshot_status = "viewing " + dir;
}

void exit_snapshot_mode() {
  g_state.snapshot_dir.clear();
  g_active_snapshot_dir.clear();
  g_snapshot_building = {};
  if (g_view)
    g_view->apply_snapshot_dir("");
  imrmf::map_editor::set_yjs_readonly(false);
  g_state.snapshot_status.clear();
}

void issue_snapshot_requests() {
#ifdef __EMSCRIPTEN__
  if (g_building_id.empty())
    return;
  if (g_state.snapshot_request_refresh) {
    g_state.snapshot_request_refresh = false;
    g_snapshots_dirty = true;
  }
  if (g_snapshots_dirty) {
    g_snapshots_dirty = false;
    imrmf_call_list_snapshots(g_server_url.c_str(), g_building_id.c_str());
  }
  if (g_state.snapshot_request_create) {
    g_state.snapshot_request_create = false;
    g_state.snapshot_status = "creating...";
    imrmf_call_create_snapshot(g_server_url.c_str(), g_building_id.c_str());
  }
  if (!g_state.snapshot_request_load.empty()) {
    std::string dir = g_state.snapshot_request_load;
    g_state.snapshot_request_load.clear();
    g_state.snapshot_status = "loading " + dir + "...";
    imrmf_call_load_snapshot_yaml(g_server_url.c_str(), g_building_id.c_str(),
                                  dir.c_str());
  }
  if (!g_state.snapshot_request_unload.empty()) {
    g_state.snapshot_request_unload.clear();
    exit_snapshot_mode();
  }
  if (!g_state.snapshot_request_restore.empty()) {
    std::string dir = g_state.snapshot_request_restore;
    g_state.snapshot_request_restore.clear();
    g_state.snapshot_status = "restoring " + dir + "...";
    imrmf_call_restore_snapshot(g_server_url.c_str(), g_building_id.c_str(),
                                dir.c_str());
  }
#endif
}

void poll_snapshot_result() {
#ifdef __EMSCRIPTEN__
  const char *code_c = imrmf_snap_result_code();
  if (!code_c)
    return;
  std::string code(code_c);
  std::free((void *)code_c);
  if (code == "idle" || code == "busy")
    return;

  const char *payload_c = imrmf_snap_result_payload();
  std::string payload = payload_c ? payload_c : "";
  if (payload_c)
    std::free((void *)payload_c);

  if (code == "list") {
    g_state.snapshots.clear();
    try {
      YAML::Node node = YAML::Load(payload);
      if (node.IsMap()) {
        YAML::Node arr = node["snapshots"];
        if (arr && arr.IsSequence()) {
          for (auto it : arr) {
            EditorState::SnapshotEntry e;
            e.dir = it["dir"].as<std::string>("");
            e.sha = it["sha"].as<std::string>("");
            e.created_at = it["created_at"].as<long long>(0);
            if (!e.dir.empty())
              g_state.snapshots.push_back(std::move(e));
          }
        }
      }
    } catch (...) {
    }
  } else if (code == "created") {
    g_state.snapshot_status = "snapshot created";
    g_snapshots_dirty = true;
  } else if (code == "yaml") {
    const char *dir_c = imrmf_snap_result_dir();
    std::string dir = dir_c ? dir_c : "";
    if (dir_c)
      std::free((void *)dir_c);
    if (!dir.empty())
      enter_snapshot_mode(dir, payload);
  } else if (code == "restored") {
    g_state.snapshot_status = "restored from " + payload;
    exit_snapshot_mode();
  } else if (code == "err") {
    g_state.snapshot_status = "error: " + payload;
  }
  imrmf_snap_reset_result();
#endif
}

void issue_branch_requests() {
#ifdef __EMSCRIPTEN__
  if (g_building_id.empty())
    return;
  if (g_state.branch_request_refresh) {
    g_state.branch_request_refresh = false;
    g_branches_dirty = true;
  }
  if (g_branches_dirty) {
    g_branches_dirty = false;
    imrmf_call_list_branches(g_server_url.c_str());
  }
  if (!g_state.deploy_request_to.empty() &&
      !g_state.deploy_request_dir.empty()) {
    std::string to = g_state.deploy_request_to;
    std::string dir = g_state.deploy_request_dir;
    g_state.deploy_request_to.clear();
    g_state.deploy_request_dir.clear();
    g_state.deploy_status = "deploying " + dir + " to " + to + "...";
    imrmf_call_deploy_snapshot(g_server_url.c_str(), g_building_id.c_str(),
                               dir.c_str(), to.c_str());
  }
  if (!g_state.deploy_latest_to.empty()) {
    std::string to = g_state.deploy_latest_to;
    g_state.deploy_latest_to.clear();
    g_state.deploy_status = "deploying latest to " + to + "...";
    imrmf_call_deploy_latest(g_server_url.c_str(), g_building_id.c_str(),
                             to.c_str());
  }
  if (!g_state.branch_switch_to.empty()) {
    std::string b = g_state.branch_switch_to;
    g_state.branch_switch_to.clear();
    if (b != g_state.branch) {
      exit_snapshot_mode();
      g_snapshots_dirty = true;
      g_view.reset();
      imrmf_call_disconnect(nullptr);
      g_state.branch = b;
      g_phase = ConnPhase::Mounting;
      imrmf_reset_result();
      imrmf_call_switch_branch(g_server_url.c_str(), b.c_str());
    }
  }
#endif
}

void poll_branch_result() {
#ifdef __EMSCRIPTEN__
  const char *code_c = imrmf_branch_result_code();
  if (!code_c)
    return;
  std::string code(code_c);
  std::free((void *)code_c);
  if (code == "idle" || code == "busy")
    return;

  const char *payload_c = imrmf_branch_result_payload();
  std::string payload = payload_c ? payload_c : "";
  if (payload_c)
    std::free((void *)payload_c);

  if (code == "list") {
    g_state.branches.clear();
    try {
      YAML::Node node = YAML::Load(payload);
      if (node.IsMap()) {
        YAML::Node arr = node["branches"];
        if (arr && arr.IsSequence()) {
          for (auto it : arr) {
            std::string b = it.as<std::string>("");
            if (!b.empty())
              g_state.branches.push_back(std::move(b));
          }
        }
      }
    } catch (...) {
    }
  } else if (code == "deployed") {
    g_state.deploy_status = payload.empty() ? "deployed" : payload;
  } else if (code == "err") {
    g_state.deploy_status = "error: " + payload;
  }
  imrmf_branch_reset_result();
#endif
}

// A new map is one empty level, so the editor has somewhere to draw.
std::string starter_building_yaml(const std::string &name) {
  Building b;
  b.name = name;
  b.levels.emplace_back();
  b.levels.back().name = "L1";
  return imrmf::map_editor::serialize_building(b);
}

// The server's MountConfig, built the same way for both front ends.
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

#ifdef __EMSCRIPTEN__
std::string take_string(const char *c) {
  if (!c)
    return {};
  std::string s(c);
  std::free((void *)c);
  return s;
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

// PUT the starter map, then load it. Follow-up is in poll_async_result.
void start_create_building(const std::string &id) {
  g_building_id = id;
  g_phase = ConnPhase::Loading;
  g_pending_create = true;
  imrmf_reset_result();
  imrmf_call_put_building(g_server_url.c_str(), id.c_str(),
                          starter_building_yaml(id).c_str());
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
    g_pending_create = false;
    imrmf_reset_result();
    return;
  }
  if (g_pending_create) {
    g_pending_create = false;
    imrmf_reset_result();
    start_load_building();
    return;
  }
  if (g_phase == ConnPhase::BootingConfig) {
    g_locked = json_bool_field(payload, "locked");
    g_state.branch = json_string_field(payload, "branch");
    std::string backend = json_string_field(payload, "backend");
    if (backend == "s3")
      g_form.kind_idx = 1;
    else if (backend == "local")
      g_form.kind_idx = 0;
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
  g_snapshots_dirty = true;
  g_branches_dirty = true;
}

#else // !__EMSCRIPTEN__

// Desktop local mode: read and write the yaml directly, no server, no CRDT.

namespace fs = std::filesystem;

std::string g_native_yaml_path;

bool looks_like_building_yaml(const fs::path &p) {
  const std::string n = p.filename().string();
  const std::string suffix = ".building.yaml";
  return n.size() > suffix.size() &&
         n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Backs the fallback browser on platforms without an OS file picker.
void native_fs_list(const std::string &path) {
  std::error_code ec;
  fs::path dir = path.empty() ? fs::current_path(ec) : fs::path(path);
  if (!fs::is_directory(dir, ec)) {
    g_fs.error = "not a directory: " + dir.string();
    return;
  }
  g_fs.error.clear();
  g_fs.current_path = dir.string();
  g_fs.parent_path = dir.has_parent_path() && dir.parent_path() != dir
                         ? dir.parent_path().string()
                         : "";

  g_fs.entries.clear();
  for (fs::directory_iterator
           it(dir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end && !ec; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (!name.empty() && name[0] == '.')
      continue;
    FsEntry e;
    e.name = name;
    e.is_dir = it->is_directory(ec);
    e.is_building_yaml = !e.is_dir && looks_like_building_yaml(it->path());
    g_fs.entries.push_back(std::move(e));
  }
  std::sort(g_fs.entries.begin(), g_fs.entries.end(),
            [](const FsEntry &a, const FsEntry &b) {
              if (a.is_dir != b.is_dir)
                return a.is_dir;
              return a.name < b.name;
            });
}

// Splits an OK:/ERR: reply and frees the string the client library owns.
bool take_client_result(char *raw, std::string *payload) {
  if (!raw) {
    if (payload)
      *payload = "no response";
    return false;
  }
  const std::string s(raw);
  imrmf_client_string_free(raw);
  const bool okay = s.rfind("OK:", 0) == 0;
  if (payload)
    *payload = s.substr(okay ? 3 : (s.rfind("ERR:", 0) == 0 ? 4 : 0));
  return okay;
}

// Browser's contract, synchronous: ends at Mounted or Error.
void start_mount() {
  g_phase = ConnPhase::Mounting;
  g_server_url = g_form.server_url;

  std::string payload;
  if (!take_client_result(imrmf_client_mount(g_server_url.c_str(),
                                             build_mount_json(g_form).c_str()),
                          &payload)) {
    g_error_message = "mount failed: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }

  g_buildings.clear();
  for (size_t start = 0; start <= payload.size();) {
    const size_t nl = payload.find('\n', start);
    const std::string id = payload.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    if (!id.empty())
      g_buildings.push_back(id);
    if (nl == std::string::npos)
      break;
    start = nl + 1;
  }
  if (g_buildings.empty()) {
    g_error_message = "no buildings there";
    g_phase = ConnPhase::Error;
    return;
  }
  if (g_building_id.empty())
    g_building_id = g_buildings.front();
  g_error_message.clear();
  g_phase = ConnPhase::Mounted;
}

std::string websocket_url(const std::string &server, const std::string &id) {
  std::string ws = server;
  if (ws.rfind("https://", 0) == 0)
    ws = "wss://" + ws.substr(8);
  else if (ws.rfind("http://", 0) == 0)
    ws = "ws://" + ws.substr(7);
  while (!ws.empty() && ws.back() == '/')
    ws.pop_back();
  return ws + "/ws/" + id;
}

// Loads the building server-side, then joins its CRDT doc over the WebSocket.
void start_load_building() {
  g_phase = ConnPhase::Loading;

  std::string payload;
  if (!take_client_result(imrmf_client_load_building(g_server_url.c_str(),
                                                     g_building_id.c_str()),
                          &payload)) {
    g_error_message = "load failed: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }
  if (!take_client_result(
          imrmf_client_connect(
              websocket_url(g_server_url, g_building_id).c_str()),
          &payload)) {
    g_error_message = "connect failed: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }

  g_native_yaml_path.clear(); // server-backed, saving goes through the CRDT
  g_state = {};
  g_view = std::make_unique<EditorView>(g_server_url, g_building_id);
  g_error_message.clear();
  g_phase = ConnPhase::Connected;
}

// Takes a *.building.yaml or a directory holding one.
bool native_open_local(const std::string &path) {
  std::error_code ec;
  fs::path p(path);
  if (fs::is_directory(p, ec)) {
    fs::path found;
    for (fs::directory_iterator it(p, ec), end; it != end && !ec;
         it.increment(ec)) {
      if (!it->is_directory(ec) && looks_like_building_yaml(it->path())) {
        found = it->path();
        break;
      }
    }
    if (found.empty()) {
      g_error_message = "no *.building.yaml in " + p.string();
      return false;
    }
    p = found;
  }

  std::ifstream in(p);
  if (!in) {
    g_error_message = "cannot read " + p.string();
    return false;
  }
  std::string body((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());

  try {
    g_building = imrmf::map_editor::parse_building(body);
  } catch (const std::exception &e) {
    g_error_message = std::string("parse failed: ") + e.what();
    return false;
  }

  g_native_yaml_path = p.string();
  g_building_id = p.stem().stem().string();
  g_state = {};
  g_view = std::make_unique<EditorView>(std::string(), g_building_id);
  g_error_message.clear();
  g_phase = ConnPhase::Connected;
  return true;
}

// A directory came from the browser, not a save panel, so name the file.
bool native_create_local(const std::string &path) {
  std::error_code ec;
  fs::path p(path);
  if (fs::is_directory(p, ec)) {
    p /= p.filename().string() + ".building.yaml";
    // The save panel asks about replacing a file, this path has nothing to ask.
    if (fs::exists(p, ec)) {
      g_error_message = p.string() + " already exists";
      return false;
    }
  }

  std::ofstream out(p, std::ios::trunc);
  if (!out) {
    g_error_message = "cannot create " + p.string();
    return false;
  }
  out << starter_building_yaml(p.stem().stem().string());
  out.close();
  return native_open_local(p.string());
}

// Create is a PUT of the starter map, then the normal load-and-sync path.
void start_create_building(const std::string &id) {
  std::string payload;
  if (!take_client_result(
          imrmf_client_put_building(g_server_url.c_str(), id.c_str(),
                                    starter_building_yaml(id).c_str()),
          &payload)) {
    g_error_message = "create failed: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }
  g_building_id = id;
  start_load_building();
}

void native_save() {
  if (g_native_yaml_path.empty())
    return;
  std::ofstream out(g_native_yaml_path, std::ios::trunc);
  if (!out) {
    g_error_message = "cannot write " + g_native_yaml_path;
    return;
  }
  out << imrmf::map_editor::serialize_building(g_building);
  g_state.dirty = false;
}

#endif // __EMSCRIPTEN__

template <size_t N> void set_field(char (&dst)[N], const std::string &src) {
  std::strncpy(dst, src.c_str(), N - 1);
  dst[N - 1] = '\0';
}

std::string join_path(const std::string &dir, const std::string &name) {
  if (dir.empty() || dir.back() == '/')
    return dir + name;
  return dir + "/" + name;
}

// Browser lists through the server, desktop reads the disk. One call site.
void request_fs_list(const std::string &path) {
#ifdef __EMSCRIPTEN__
  start_fs_list(path);
#else
  native_fs_list(path);
#endif
}

// Fallback picker, for the browser and for platforms with no OS panel.
void draw_fs_browser(float height) {
  if (!g_fs.requested_once) {
    g_fs.requested_once = true;
    request_fs_list("");
  }
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
    if (ImGui::SmallButton(".."))
      request_fs_list(g_fs.parent_path);
    ImGui::SameLine();
  }
  if (ImGui::SmallButton("refresh"))
    request_fs_list(g_fs.current_path);
  ImGui::SameLine();
  if (ImGui::SmallButton("use current"))
    set_field(g_form.local_path, g_fs.current_path);

  ImGui::Spacing();
  ImGui::BeginChild("##fs_entries", ImVec2(0, height), true);
  for (const auto &e : g_fs.entries) {
    const std::string full = join_path(g_fs.current_path, e.name);
    if (e.is_dir) {
      if (ImGui::Selectable(("[d] " + e.name).c_str(), false))
        request_fs_list(full);
      continue;
    }
    const ImVec4 col = e.is_building_yaml ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                          : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
#ifndef __EMSCRIPTEN__
    // Desktop opens the file itself, so let the user click one.
    if (e.is_building_yaml) {
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      if (ImGui::Selectable(("    " + e.name).c_str(), false))
        set_field(g_form.local_path, full);
      ImGui::PopStyleColor();
      continue;
    }
#endif
    ImGui::TextColored(col, "    %s%s", e.name.c_str(),
                       e.is_building_yaml ? " (building.yaml)" : "");
  }
  ImGui::EndChild();
}

// One dialog for both front ends. Only the OS file picker differs.
void draw_open_dialog_body() {
  const bool mounted = !g_buildings.empty();
  const bool local = g_form.kind_idx == 0;
  ImGui::TextDisabled("Choose where your building.yaml lives.");
  ImGuiWidgets::SectionGap();
#ifdef __EMSCRIPTEN__
  const bool os_picker = false;
#else
  const bool os_picker = local && has_native_file_picker();
#endif

  if (!g_locked) {
    // Form table, so long labels cannot widen the dialog.
    if (ImGuiWidgets::BeginFormTable("##open_form")) {
      // A local file on the desktop is opened directly, so no server involved.
      if (!os_picker) {
        ImGuiWidgets::FormRow("Server URL");
        ImGui::SetNextItemWidth(-FLT_MIN); // fills the cell
        ImGui::InputText("##server_url", g_form.server_url,
                         sizeof(g_form.server_url));
#ifdef __EMSCRIPTEN__
        ImGui::SameLine();
        if (ImGui::SmallButton("origin"))
          set_field(g_form.server_url, take_string(imrmf_default_server_url()));
#endif
      }

      ImGuiWidgets::FormRow("Storage");
      const int kind = ImGuiWidgets::ButtonGroupSelector(
          {"Local file", "S3 Bucket"}, g_form.kind_idx, ImVec2(0, 0));
      if (kind >= 0 && kind != g_form.kind_idx) {
        g_form.kind_idx = kind;
        g_buildings.clear();
        g_error_message.clear();
      }

      if (!local) {
        struct Field {
          const char *label;
          const char *id;
          char *buffer;
          size_t size;
          ImGuiInputTextFlags flags;
        };
        const Field fields[] = {
            {"Bucket", "##bucket", g_form.s3_bucket, sizeof(g_form.s3_bucket),
             0},
            {"Prefix", "##prefix", g_form.s3_prefix, sizeof(g_form.s3_prefix),
             0},
            {"Region", "##region", g_form.s3_region, sizeof(g_form.s3_region),
             0},
            {"Access key id", "##access", g_form.s3_access,
             sizeof(g_form.s3_access), 0},
            {"Secret access key", "##secret", g_form.s3_secret,
             sizeof(g_form.s3_secret), ImGuiInputTextFlags_Password},
            {"Endpoint url", "##endpoint", g_form.s3_endpoint,
             sizeof(g_form.s3_endpoint), 0},
        };
        for (const Field &f : fields) {
          ImGuiWidgets::FormRow(f.label);
          ImGui::SetNextItemWidth(-FLT_MIN);
          ImGui::InputText(f.id, f.buffer, f.size, f.flags);
        }
      }

      ImGui::EndTable();
    }

    if (local) {
      ImGuiWidgets::SectionGap();
      if (os_picker) {
        ImGui::TextWrapped("Open a building.yaml, or create one. Edits are "
                           "written straight back to the file.");
        if (g_form.local_path[0])
          ImGui::TextDisabled("%s", g_form.local_path);
      } else {
        draw_fs_browser(220.0f);
      }
    } else if (!mounted) {
      ImGui::TextDisabled(
          "Leave the endpoint empty for AWS, set it for minio.");
    }
  }

  // Outside the form: locked mode hides that but still needs the picker.
  if (mounted && ImGuiWidgets::BeginFormTable("##open_building")) {
    ImGuiWidgets::FormRow("Building");
    int selected = 0;
    for (int i = 0; i < (int)g_buildings.size(); ++i) {
      if (g_buildings[i] == g_building_id)
        selected = i;
    }
    std::vector<const char *> names;
    names.reserve(g_buildings.size());
    for (const auto &b : g_buildings)
      names.push_back(b.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##building", &selected, names.data(), (int)names.size()))
      g_building_id = g_buildings[selected];

    if (!g_locked) {
      ImGuiWidgets::FormRow("New name");
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputText("##new_name", g_form.building_id,
                       sizeof(g_form.building_id));
    }
    ImGui::EndTable();
  }

  if (mounted && !g_locked) {
    ImGuiWidgets::SectionGap();
    if (ImGui::SmallButton("Use a different backend")) {
#ifdef __EMSCRIPTEN__
      disconnect_and_reset();
#else
      g_buildings.clear();
      g_error_message.clear();
      g_phase = ConnPhase::Modal;
#endif
      return;
    }
  }

  if (!g_error_message.empty()) {
    ImGui::Spacing();
    ImGuiWidgets::StatusLine(theme::Signal::danger, g_error_message.c_str());
  }

  // Create: OS save panel for a local file, otherwise a PUT.
  const char *primary = (os_picker || mounted) ? "Open" : "Connect";
  // Locked mode hides the name field, leaving Create nothing to use.
  const char *secondary =
      (os_picker || mounted) && !g_locked ? "Create" : nullptr;

  bool ready = true;
  if (!g_locked && !mounted) {
    ready =
        os_picker || (local ? g_form.local_path[0] != '\0'
                            : g_form.s3_bucket[0] && g_form.s3_region[0] &&
                                  g_form.s3_access[0] && g_form.s3_secret[0]);
  }

  switch (ImGuiWidgets::ModalActions(primary, secondary, ready)) {
  case 1:
#ifndef __EMSCRIPTEN__
    // Desktop reads local files off disk. No server either way.
    if (local) {
      std::string path = g_form.local_path;
      if (has_native_file_picker()) {
        path = pick_building_yaml();
        if (path.empty())
          return;
        set_field(g_form.local_path, path);
      }
      if (!path.empty())
        native_open_local(path);
      return;
    }
#endif
    if (mounted)
      start_load_building();
    else
      start_mount();
    return;
  case 2:
#ifndef __EMSCRIPTEN__
    if (local) {
      std::string path = g_form.local_path;
      if (has_native_file_picker()) {
        path = pick_new_building_yaml();
        if (path.empty())
          return;
        set_field(g_form.local_path, path);
      }
      if (path.empty())
        g_error_message = "choose where the new file should go";
      else
        native_create_local(path);
      return;
    }
#endif
    if (g_form.building_id[0])
      start_create_building(g_form.building_id);
    else
      g_error_message = "give the new building a name";
    return;
  default:
    return;
  }
}

// Floating version, for the browser where the dialog sits over the page.
void draw_connection_modal() {
  if (!ImGui::IsPopupOpen("Open a building##imrmf"))
    ImGui::OpenPopup("Open a building##imrmf");
  if (ImGuiWidgets::BeginModal("Open a building##imrmf", 560.0f)) {
    draw_open_dialog_body();
    ImGuiWidgets::EndModal();
  }
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
    summary = "Local";
    if (g_form.local_path[0]) {
      summary += " \xC2\xB7 ";
      summary += g_form.local_path;
    }
  } else {
    summary = "S3";
    if (g_form.s3_bucket[0]) {
      summary += " \xC2\xB7 ";
      summary += g_form.s3_bucket;
      if (g_form.s3_prefix[0]) {
        summary += "/";
        summary += g_form.s3_prefix;
      }
    }
  }
  if (!g_building_id.empty()) {
    summary += " \xC2\xB7 ";
    summary += g_building_id;
  }
  h.connection_label = std::move(summary);

  std::string details;
  auto add = [&](const char *k, const std::string &v) {
    if (!v.empty()) {
      details += k;
      details += ": ";
      details += v;
      details += "\n";
    }
  };
  add("Building", g_building_id);
  if (g_form.kind_idx == 0) {
    add("Backend", "Local");
    add("Path", g_form.local_path);
  } else {
    add("Backend", "S3");
    add("Bucket", g_form.s3_bucket);
    add("Prefix", g_form.s3_prefix);
    add("Region", g_form.s3_region);
    add("Endpoint", g_form.s3_endpoint);
  }
  add("Branch", g_state.branch);
  if (!details.empty() && details.back() == '\n')
    details.pop_back();
  h.details = std::move(details);

  h.can_disconnect = !g_locked;
  h.on_disconnect = []() {
#ifdef __EMSCRIPTEN__
    disconnect_and_reset();
#endif
  };
  return h;
}

void begin_frame() {
  glfwPollEvents();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void end_frame() {
  ImGui::Render();
  int w, h;
  glfwGetFramebufferSize(g_window, &w, &h);
  glViewport(0, 0, w, h);
  glClearColor(theme::palette::bg.x, theme::palette::bg.y, theme::palette::bg.z,
               1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifndef __EMSCRIPTEN__
  glfwSwapBuffers(g_window);
#endif
}

void frame() {
  begin_frame();

#ifdef __EMSCRIPTEN__
  poll_async_result();
  poll_fs_result();
  poll_snapshot_result();
  poll_branch_result();
#endif

  if (g_phase == ConnPhase::Connected) {
    mirror_from_yjs();
    issue_snapshot_requests();
    issue_branch_requests();
#ifndef __EMSCRIPTEN__
    if (ImGui::GetIO().KeySuper || ImGui::GetIO().KeyCtrl) {
      if (ImGui::IsKeyPressed(ImGuiKey_S, false))
        native_save();
    }
#endif

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
    Building &shown =
        g_state.snapshot_dir.empty() ? g_building : g_snapshot_building;
    if (g_view && !shown.levels.empty()) {
      g_view->draw(shown, g_state, []() {}, build_top_bar_hooks());
    } else {
      ImGui::Text("Waiting for initial sync...");
    }
    ImGui::End();
    if (g_state.snapshot_dir.empty())
      push_to_yjs_if_dirty();
  }
#ifdef __EMSCRIPTEN__
  // Desktop picks its building in the launcher, so it arrives here connected.
  else if (g_phase == ConnPhase::Modal || g_phase == ConnPhase::Error ||
           g_phase == ConnPhase::Mounted) {
    draw_connection_modal();
  } else if (g_phase == ConnPhase::BootingConfig) {
    draw_busy("contacting server...");
  } else if (g_phase == ConnPhase::Mounting) {
    draw_busy(g_locked ? "loading server state..." : "mounting backend...");
  } else if (g_phase == ConnPhase::Loading) {
    draw_busy("loading building...");
  } else if (g_phase == ConnPhase::Connecting) {
    draw_busy("connecting...");
  }
#endif

  end_frame();
}

GLFWwindow *create_window(int w, int h, const char *title, bool decorated,
                          bool resizable = true) {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
#ifdef __APPLE__
  // macOS only offers 3.2+ core, and only forward-compatible.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  glfwWindowHint(GLFW_DECORATED, decorated ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, resizable ? GLFW_TRUE : GLFW_FALSE);
  GLFWwindow *win = glfwCreateWindow(w, h, title, nullptr, nullptr);
  if (!win)
    std::fprintf(stderr, "glfw window failed\n");
  return win;
}

void init_backends(GLFWwindow *win) {
  glfwMakeContextCurrent(win);
  ImGui_ImplGlfw_InitForOpenGL(win, true);
#ifdef __EMSCRIPTEN__
  ImGui_ImplOpenGL3_Init("#version 300 es");
#elif defined(__APPLE__)
  ImGui_ImplOpenGL3_Init("#version 150");
#else
  ImGui_ImplOpenGL3_Init("#version 330");
#endif
}

void shutdown_backends() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
}

#ifndef __EMSCRIPTEN__

void center_window(GLFWwindow *win, int w, int h) {
  int mx = 0, my = 0, mw = 0, mh = 0;
  glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
  if (mw > 0 && mh > 0)
    glfwSetWindowPos(win, mx + (mw - w) / 2, my + (mh - h) / 2);
}

// Runs before the editor window. The window follows the modal's size.
bool run_launcher() {
  const int width = 560;
  // Decorated to keep the shadow and rounded corners.
  g_window = create_window(width, 240, "Open a building", /*decorated=*/true,
                           /*resizable=*/false);
  if (!g_window)
    return false;
  if (has_custom_titlebar())
    use_custom_titlebar(g_window);
  init_backends(g_window);

  // The modal is the window, so this padding is just a strip around it.
  const ImVec2 safe_area = ImGui::GetStyle().DisplaySafeAreaPadding;
  ImGui::GetStyle().DisplaySafeAreaPadding = ImVec2(0.0f, 0.0f);

  bool centred = false;
  int last_height = 0;
  while (g_phase != ConnPhase::Connected && !glfwWindowShouldClose(g_window)) {
    begin_frame();
    // Re-opening every frame would reset the auto-fit.
    if (!ImGui::IsPopupOpen("Open a building##imrmf_launcher"))
      ImGui::OpenPopup("Open a building##imrmf_launcher");
    float wanted_height = 0.0f;
    if (ImGuiWidgets::BeginModal("Open a building##imrmf_launcher",
                                 (float)width,
                                 /*fill_host=*/true)) {
      if (has_custom_titlebar()) {
        // Screen space, or the window's own movement feeds back in.
        static bool was_held = false;
        static double grab_x = 0.0, grab_y = 0.0;
        const bool held = ImGuiWidgets::WindowTitleBar("Open a building");
        double cursor_x = 0.0, cursor_y = 0.0;
        if (held && global_cursor_position(&cursor_x, &cursor_y)) {
          int x = 0, y = 0;
          glfwGetWindowPos(g_window, &x, &y);
          if (!was_held) {
            grab_x = cursor_x - x;
            grab_y = cursor_y - y;
          } else {
            glfwSetWindowPos(g_window, (int)(cursor_x - grab_x),
                             (int)(cursor_y - grab_y));
          }
        }
        was_held = held;
        ImGuiWidgets::SectionGap();
      }
      draw_open_dialog_body();
      // GetWindowSize() is clamped to the host, so it could only shrink.
      const ImGuiWindow *win = ImGui::GetCurrentWindow();
      wanted_height = win->ContentSizeIdeal.y + win->WindowPadding.y * 2.0f +
                      win->DecoOuterSizeY1 + win->DecoOuterSizeY2;
      ImGuiWidgets::EndModal();
    }
    end_frame();

    // A fresh popup reports a placeholder size, and resizing to it sticks.
    int current_w = 0, current_h = 0;
    glfwGetWindowSize(g_window, &current_w, &current_h);
    const int height = (int)(wanted_height + 0.5f);
    if (height > 1 && height == last_height && height != current_h) {
      glfwSetWindowSize(g_window, width, height);
      // Only the first fit centres, later ones may have been dragged.
      if (!centred) {
        center_window(g_window, width, height);
        centred = true;
      }
    }
    last_height = height;
  }

  ImGui::GetStyle().DisplaySafeAreaPadding = safe_area;
  shutdown_backends();
  glfwDestroyWindow(g_window);
  g_window = nullptr;
  return g_phase == ConnPhase::Connected;
}

#endif // __EMSCRIPTEN__

} // namespace

int main(int argc, char **argv) {
  if (!glfwInit()) {
    std::fprintf(stderr, "glfw init failed\n");
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  // Beside the binary: the working directory is not ours to assume.
  const std::string exe = argc > 0 && argv[0] ? argv[0] : "";
  const std::string inter = exe + ".runfiles/_main/ui/fonts/Inter.ttf";
  const std::string mdi =
      exe + ".runfiles/_main/ui/fonts/materialdesignicons-webfont.ttf";

  theme::apply();
  theme::load_default_font(16.0f,
                           {"Inter.ttf", inter.c_str(), "ui/fonts/Inter.ttf"});
  theme::load_icons(16.0f,
                    {ICON_MDI_CURSOR_DEFAULT,
                     ICON_MDI_VECTOR_POINT,
                     ICON_MDI_VECTOR_POLYLINE,
                     ICON_MDI_WALL,
                     ICON_MDI_DOOR,
                     ICON_MDI_RULER,
                     ICON_MDI_TEXTURE_BOX,
                     ICON_MDI_VECTOR_POLYGON_VARIANT,
                     ICON_MDI_LAYERS_EDIT,
                     ICON_MDI_UNDO,
                     ICON_MDI_REDO,
                     ICON_MDI_DELETE,
                     ICON_MDI_ALIGN_HORIZONTAL_CENTER,
                     ICON_MDI_ALIGN_VERTICAL_CENTER,
                     ICON_MDI_CLOSE,
                     ICON_MDI_ACCOUNT_MULTIPLE,
                     ICON_MDI_PLUS,
                     ICON_MDI_MOVE_RESIZE,
                     ICON_MDI_CHECK,
                     ICON_MDI_LAYERS,
                     ICON_MDI_FOLDER_OPEN,
                     ICON_MDI_FOLDER,
                     ICON_MDI_FILE_IMAGE,
                     ICON_MDI_ARROW_UP,
                     ICON_MDI_REFRESH,
                     ICON_MDI_UPLOAD},
                    {"materialdesignicons-webfont.ttf", mdi.c_str(),
                     "ui/fonts/materialdesignicons-webfont.ttf"});

#ifdef __EMSCRIPTEN__
  g_window = create_window(1600, 900, "ImRmfMapEditor", true);
  if (!g_window) {
    glfwTerminate();
    return 1;
  }
  init_backends(g_window);
  ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_window, "#canvas");
  imrmf_init_state();
  start_boot_config();
  emscripten_set_main_loop(frame, 0, 1);
#else
  // Pick the building first. The editor window opens only once that succeeds.
  if (!run_launcher()) {
    ImGui::DestroyContext();
    glfwTerminate();
    return 0;
  }

  g_window = create_window(1600, 900, "ImRmfMapEditor", true);
  if (!g_window) {
    glfwTerminate();
    return 1;
  }
  init_backends(g_window);
  while (!glfwWindowShouldClose(g_window))
    frame();
  shutdown_backends();
  glfwDestroyWindow(g_window);
#endif

  ImGui::DestroyContext();
  glfwTerminate();
  return 0;
}
