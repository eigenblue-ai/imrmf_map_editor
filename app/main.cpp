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

#include "canvas/http_texture_provider.hpp"
#include "canvas/stb_texture_provider.hpp"
#include "model/asset_paths.hpp"
#include "model/building.hpp"
#include "model/edit_history.hpp"
#include "model/map_bundle.hpp"
#include "model/yaml_io.hpp"
#include "view/editor_view.hpp"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
using imrmf::map_editor::BundleError;
using imrmf::map_editor::EditorState;
using imrmf::map_editor::EditorView;
using imrmf::map_editor::MapBundle;
namespace mecanvas = imrmf::map_editor::canvas;
#ifndef __EMSCRIPTEN__
using imrmf::map_editor::FileKind;
using imrmf::map_editor::global_cursor_position;
using imrmf::map_editor::has_custom_titlebar;
using imrmf::map_editor::has_native_file_picker;
using imrmf::map_editor::pick_open_path;
using imrmf::map_editor::pick_save_path;
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
#ifdef __EMSCRIPTEN__
bool g_snapshots_dirty = true;
bool g_branches_dirty = true;
#endif

// Set when the map came from a .rmfmap bundle. Ctrl+S rewrites that file.
std::string g_native_bundle_path;
std::shared_ptr<mecanvas::AssetBlobs> g_bundle_blobs;
// Nothing to mount, mirror, snapshot or branch, and no CRDT to push into.
bool g_bundle_session = false; // the map came from a .rmfmap, not a backend
std::string g_bundle_name;     // what to call it in the UI
bool g_serverless_session = false;

// A desktop client has no page origin to resolve the asset route against, so
// it has to name the server.
std::unique_ptr<mecanvas::TextureProvider> make_server_texture_provider() {
  auto p = std::make_unique<mecanvas::HttpTextureProvider>();
#ifndef __EMSCRIPTEN__
  p->set_base_url(g_server_url);
#endif
  return p;
}

// The rule the server enforces. The id ends up in a URL, an S3 key and a path
// under the cache root.
bool building_id_is_valid(const std::string &id) {
  if (id.empty() || id.size() > 64)
    return false;
  for (unsigned char c : id) {
    const bool okay = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!okay)
      return false;
  }
  return true;
}

// A map picked off disk, ready to push. Never the file's own bytes: the yaml is
// re-serialized from what the parser accepted.
struct MapSource {
  bool ok = false;
  std::string error;
  std::string yaml;
  std::vector<imrmf::map_editor::BundleAsset> assets;
  std::vector<std::string> missing;
};

// A building.yaml is never this big, and read_bundle bounds a bundle again.
constexpr std::size_t kMaxYamlBytes = 32u * 1024u * 1024u;

MapSource read_map_source(const std::vector<unsigned char> &bytes,
                          bool as_bundle) {
  MapSource out;
  if (bytes.empty()) {
    out.error = "file is empty";
    return out;
  }

  imrmf::map_editor::Building b;
  if (as_bundle) {
    try {
      imrmf::map_editor::MapBundle bundle =
          imrmf::map_editor::read_bundle(bytes.data(), bytes.size());
      b = std::move(bundle.building);
      out.assets = std::move(bundle.assets);
      out.missing = std::move(bundle.missing);
    } catch (const std::exception &e) {
      out.error = std::string("not a usable map bundle: ") + e.what();
      return out;
    }
  } else {
    if (bytes.size() > kMaxYamlBytes) {
      out.error = "yaml is too large to be a map";
      return out;
    }
    try {
      b = imrmf::map_editor::parse_building(
          std::string(bytes.begin(), bytes.end()));
    } catch (const std::exception &e) {
      out.error = std::string("parse failed: ") + e.what();
      return out;
    }
  }

  if (b.levels.empty()) {
    out.error = "no levels in that map";
    return out;
  }
  // A path the backend cannot address relatively would land where the map
  // cannot read it back.
  for (const std::string *ref : imrmf::map_editor::building_asset_refs(b)) {
    if (!imrmf::map_editor::asset_path_is_portable(*ref)) {
      out.error = "image path leaves the map folder: " + *ref;
      return out;
    }
  }
  for (const imrmf::map_editor::BundleAsset &a : out.assets) {
    if (!imrmf::map_editor::asset_path_is_portable(a.path)) {
      out.error = "bundled image has an unusable path: " + a.path;
      return out;
    }
  }

  out.yaml = imrmf::map_editor::serialize_building(b);
  out.ok = true;
  return out;
}

std::string bundle_yaml_name() {
  return (g_building_id.empty() ? std::string("map") : g_building_id) +
         ".building.yaml";
}

#ifdef __EMSCRIPTEN__
std::vector<std::string> current_asset_paths() {
  std::vector<std::string> out;
  for (const std::string *ref :
       imrmf::map_editor::building_asset_refs(g_building)) {
    if (std::find(out.begin(), out.end(), *ref) == out.end())
      out.push_back(*ref);
  }
  return out;
}
#endif // __EMSCRIPTEN__

MapBundle pack_current_bundle(const imrmf::map_editor::AssetReader &read) {
  return imrmf::map_editor::collect_bundle(g_building, bundle_yaml_name(),
                                           read);
}

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

// Stands in for a credential the server holds but never sends.
constexpr const char *kSecretMask = "\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F"
                                    "\xE2\x97\x8F\xE2\x97\x8F\xE2\x97\x8F"
                                    "\xE2\x97\x8F\xE2\x97\x8F";

struct ConnectionForm {
  int kind_idx = 0;         // 0 = Local, 1 = S3
  int local_format_idx = 0; // 0 = building.yaml, 1 = full map (.rmfmap)
  // True while a credential field still shows the server's masked value.
  bool s3_secret_is_server_held = false;
  bool s3_access_is_server_held = false;
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
// Attached, which is not the same as holding anything. Read off the building
// list, an empty backend looked unmounted and could never be filled.
bool g_backend_mounted = false;
std::string g_error_message;

bool g_locked = false;
// The server holds a credential we can fall back to, so the form needs none.
bool g_server_has_s3_secret = false;
bool g_server_has_s3_access = false;
// Desktop: closing a file returns to the launcher instead of quitting the app.
#ifndef __EMSCRIPTEN__
bool g_request_relaunch = false;
#endif

// Building picker mode once a backend is up: 0 = open existing, 1 = create new.
int g_building_mode = 0;

// Create-new can start from an existing map instead of the blank starter.
bool g_create_from_file = false;
// 0 = building.yaml, 1 = full map (.rmfmap). The choice decides how the file is
// read, so a mislabelled extension cannot steer it.
int g_create_format_idx = 0;
char g_create_source[512] = "";
std::string g_create_status;
// The picked file's bytes in the browser, where there is no path to re-read.
std::vector<unsigned char> g_create_bytes;

std::string connection_summary() {
  std::string out;
  if (g_form.kind_idx == 0) {
    out = g_form.local_path[0] ? g_form.local_path : "local files";
  } else {
    out = g_form.s3_bucket[0] ? g_form.s3_bucket : "S3";
    if (g_form.s3_prefix[0]) {
      out += "/";
      out += g_form.s3_prefix;
    }
    if (!g_state.branch.empty()) {
      out += " \xC2\xB7 ";
      out += g_state.branch;
    }
  }
  out += " \xC2\xB7 ";
  out += std::to_string(g_buildings.size());
  out += g_buildings.size() == 1 ? " building" : " buildings";
  return out;
}
// Browser only: a PUT is in flight and should be followed by a load.
#ifdef __EMSCRIPTEN__
bool g_pending_create = false;
#endif
std::string g_auto_building;

struct FsEntry {
  std::string name;
  bool is_dir = false;
  bool is_building_yaml = false;
  bool is_map_bundle = false;
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

// Snapshots, branches and sync state are browser-only, so the desktop build
// has no counterpart for those EM_JS calls.

#endif

// JS hands back malloc'd strings, the Rust client hands back its own.
void free_bridge_string(const char *s) {
#ifdef __EMSCRIPTEN__
  std::free((void *)s);
#else
  imrmf_client_string_free((char *)s);
#endif
}

void commit_pending_edits();

// Selections are indices into the current level, so anything that swaps the
// map under the editor has to bring them back in range.
void clamp_state_to_building() {
  if (g_building.levels.empty())
    return;
  g_state.level_idx = std::max(
      0, std::min(g_state.level_idx, (int)g_building.levels.size() - 1));
  const auto &cur = g_building.levels[g_state.level_idx];
  auto clamp_sel = [](std::vector<int> &sel, int n) {
    sel.erase(std::remove_if(sel.begin(), sel.end(),
                             [&](int i) { return i < 0 || i >= n; }),
              sel.end());
  };
  clamp_sel(g_state.selected_vertices, (int)cur.vertices.size());
  clamp_sel(g_state.selected_lanes, (int)cur.lanes.size());
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
}

// When the current burst of edits started. Zero while nothing is pending.
double g_dirty_since = 0.0;

double clock_seconds() {
#ifdef __EMSCRIPTEN__
  return emscripten_get_now() * 0.001;
#else
  return glfwGetTime();
#endif
}

#ifndef __EMSCRIPTEN__
// A file session keeps its undo stack in memory. With no peers to merge with,
// the CRDT only bought it a full yaml round trip of the map per edit.
imrmf::map_editor::EditHistory g_history;

void commit_history() {
  if (!g_state.dirty)
    return;
  g_history.commit(g_building);
  g_state.dirty = false;
  g_dirty_since = 0.0;
}

void history_undo() {
  if (g_history.undo(g_building))
    clamp_state_to_building();
}

void history_redo() {
  if (g_history.redo(g_building))
    clamp_state_to_building();
}
#endif

void mirror_from_yjs() {
  if (!imrmf_yjs_remote_dirty())
    return;
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
      ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
      ImGui::GetIO().WantTextInput) {
    return;
  }
  // Mirroring replaces our copy, so a pending edit has to land first or the
  // remote update erases it.
  commit_pending_edits();
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
      // The CRDT stores levels in a map, so the order they come back in is not
      // ours to rely on. Follow the level by name, or a mirror silently moves
      // the view to a different floor.
      std::string level_name;
      if (g_state.level_idx >= 0 &&
          g_state.level_idx < (int)g_building.levels.size())
        level_name = g_building.levels[g_state.level_idx].name;

      g_building = std::move(b);

      int found = -1;
      for (int i = 0; i < (int)g_building.levels.size(); ++i) {
        if (g_building.levels[i].name == level_name) {
          found = i;
          break;
        }
      }
      if (found >= 0)
        g_state.level_idx = found;
      clamp_state_to_building();
      g_state.dirty = false;
    }
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[imrmf] yjs mirror parse failed: %s\n", e.what());
  }
  imrmf_yjs_clear_remote_dirty();
}

// A file session snapshots the edit, a server session pushes the map into the
// shared document.
void commit_pending_edits() {
  if (!g_state.dirty)
    return;
#ifndef __EMSCRIPTEN__
  if (g_serverless_session) {
    commit_history();
    return;
  }
#endif
  std::string yaml = imrmf::map_editor::serialize_building(g_building);
  imrmf_yjs_push_local_yaml(yaml.c_str());
  g_state.dirty = false;
  g_dirty_since = 0.0;
}

// Recording costs a snapshot or a yaml round trip, so it waits for the burst
// to settle rather than running in the frame the edit lands in. A drag then
// costs one step, not one per frame.
void commit_pending_edits_if_settled() {
  if (!g_state.dirty) {
    g_dirty_since = 0.0;
    return;
  }
  const double now = clock_seconds();
  if (g_dirty_since == 0.0)
    g_dirty_since = now;

  constexpr double kSettle = 0.12;  // quiet before a push
  constexpr double kMaxHold = 0.75; // but a long drag still syncs as it runs
  const bool editing = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                       ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                       ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                       ImGui::GetIO().WantTextInput;
  const double held = now - g_dirty_since;
  if (held < (editing ? kMaxHold : kSettle))
    return;
  commit_pending_edits();
}

#ifdef __EMSCRIPTEN__
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
#endif

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

#ifdef __EMSCRIPTEN__
void poll_snapshot_result() {
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
}
#endif

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

#ifdef __EMSCRIPTEN__
void poll_branch_result() {
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
}
#endif

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
  // An untouched mask submits empty, and the server reuses its own value.
  j += ",\"access_key_id\":\"" +
       (f.s3_access_is_server_held ? std::string() : esc(f.s3_access)) + "\"";
  j += ",\"secret_access_key\":\"" +
       (f.s3_secret_is_server_held ? std::string() : esc(f.s3_secret)) + "\"";
  if (f.s3_endpoint[0])
    j += ",\"endpoint_url\":\"" + esc(f.s3_endpoint) + "\"";
  j += "}";
  return j;
}

template <size_t N> void set_field(char (&dst)[N], const std::string &src);

// Same as set_field for a buffer whose size is only known at runtime.
void copy_into(char *dst, std::size_t n, const std::string &src) {
  if (!dst || n == 0)
    return;
  const std::size_t len = std::min(src.size(), n - 1);
  std::memcpy(dst, src.data(), len);
  dst[len] = '\0';
}

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

// Applies GET /config. Credentials are never in the payload, only whether the
// server holds one, so those fields get a mask.
void apply_server_config(const std::string &payload) {
  g_locked = json_bool_field(payload, "locked");
  g_state.branch = json_string_field(payload, "branch");
  const std::string backend = json_string_field(payload, "backend");
  if (backend == "s3")
    g_form.kind_idx = 1;
  else if (backend == "local")
    g_form.kind_idx = 0;

  if (backend == "s3") {
    set_field(g_form.s3_bucket, json_string_field(payload, "bucket"));
    set_field(g_form.s3_prefix, json_string_field(payload, "prefix"));
    set_field(g_form.s3_region, json_string_field(payload, "region"));
    set_field(g_form.s3_endpoint, json_string_field(payload, "endpoint_url"));
    // Both submit empty, which the server reads as "reuse what you have".
    g_server_has_s3_secret = json_bool_field(payload, "has_secret_access_key");
    g_server_has_s3_access = json_bool_field(payload, "has_access_key_id");
    g_form.s3_secret_is_server_held = g_server_has_s3_secret;
    g_form.s3_access_is_server_held = g_server_has_s3_access;
    if (g_form.s3_secret_is_server_held)
      set_field(g_form.s3_secret, kSecretMask);
    if (g_form.s3_access_is_server_held)
      set_field(g_form.s3_access, kSecretMask);
  } else if (backend == "local") {
    set_field(g_form.local_path, json_string_field(payload, "path"));
  }
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
  g_serverless_session = false;
  g_bundle_session = false;
  g_view = std::make_unique<EditorView>(make_server_texture_provider(),
                                        g_building_id);
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
    apply_server_config(payload);
    g_auto_building = json_string_field(payload, "auto_building");
    // A server that mounted at startup already has a backend, locked or not.
    const bool has_backend = !json_string_field(payload, "backend").empty();
    if (g_locked || (has_backend && !g_auto_building.empty())) {
      start_list_buildings();
    } else {
      g_phase = ConnPhase::Modal;
      try_restore_session();
    }
  } else if (g_phase == ConnPhase::Mounting) {
    parse_buildings_payload(payload);
    g_backend_mounted = true;
    if (g_buildings.empty()) {
      // Nothing to open is not an error, it is a new backend.
      g_error_message.clear();
      g_phase = ConnPhase::Mounted;
    } else {
      g_phase = ConnPhase::Mounted;
      if (!g_auto_building.empty() &&
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
  commit_pending_edits();
  imrmf_session_clear();
  imrmf_call_disconnect(g_server_url.c_str());
  // g_locked stays, since the server still rejects /mount in single-mount mode.
  g_phase = ConnPhase::Modal;
  g_view.reset();
  g_building = {};
  g_state = {};
  g_building_id.clear();
  g_buildings.clear();
  g_backend_mounted = false;
  g_error_message.clear();
  g_snapshots_dirty = true;
  g_branches_dirty = true;
}

#else // !__EMSCRIPTEN__

// Desktop local mode: read and write the yaml directly, no server, no CRDT.

namespace fs = std::filesystem;

std::string g_native_yaml_path;

bool has_suffix(const std::string &n, const std::string &suffix) {
  return n.size() > suffix.size() &&
         n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool looks_like_building_yaml(const fs::path &p) {
  return has_suffix(p.filename().string(), ".building.yaml");
}

bool looks_like_map_bundle(const fs::path &p) {
  return has_suffix(p.filename().string(), imrmf::map_editor::kBundleExtension);
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
    e.is_map_bundle = !e.is_dir && looks_like_map_bundle(it->path());
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

// The desktop equivalent of the browser's boot fetch, for prefilling the form.
void native_probe_server_config() {
  if (g_form.server_url[0] == '\0')
    return;
  std::string payload;
  if (take_client_result(imrmf_client_fetch_config(g_form.server_url),
                         &payload)) {
    const std::string saved_url = g_form.server_url;
    apply_server_config(payload);
    set_field(g_form.server_url, saved_url);
  }
}

// Browser's contract, synchronous: ends at Mounted or Error.
// Newline-separated ids, which is what the client hands back for both a mount
// and a plain listing.
void take_building_ids(const std::string &payload);

// A server that mounted at startup answers POST /mount with 409, so list what
// it already has instead. Without this the desktop cannot reach one at all.
void start_list_buildings() {
  g_phase = ConnPhase::Mounting;
  g_server_url = g_form.server_url;

  std::string payload;
  if (!take_client_result(imrmf_client_list_buildings(g_server_url.c_str()),
                          &payload)) {
    g_error_message = "could not list buildings: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }
  take_building_ids(payload);
}

void start_mount() {
  if (g_locked) {
    start_list_buildings();
    return;
  }
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
  take_building_ids(payload);
}

void take_building_ids(const std::string &payload) {
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
  g_backend_mounted = true;
  if (g_buildings.empty()) {
    g_error_message.clear();
    g_phase = ConnPhase::Mounted;
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
  g_native_bundle_path.clear();
  g_bundle_blobs.reset();
  g_serverless_session = false;
  g_bundle_session = false;
  g_state = {};
  g_view = std::make_unique<EditorView>(make_server_texture_provider(),
                                        g_building_id);
  g_error_message.clear();
  g_phase = ConnPhase::Connected;
}

bool read_file_bytes(const fs::path &p, std::vector<unsigned char> *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return false;
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  return true;
}

// Takes a *.building.yaml or a directory holding one.
// A file session has no server and no peers, so undo rewinds snapshots in
// memory rather than a CRDT document.
void start_local_undo_session() {
  imrmf_client_disconnect();
  g_history.reset(g_building);
}

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

  // Rewrite absolute paths that live under the yaml so the map stays movable.
  const fs::path base = p.parent_path();
  for (std::string *ref : imrmf::map_editor::building_asset_refs(g_building))
    *ref = imrmf::map_editor::relativize_asset_path(*ref, base);

  g_native_yaml_path = p.string();
  g_native_bundle_path.clear();
  g_bundle_blobs.reset();
  g_serverless_session = true;
  g_bundle_session = false;
  g_bundle_name.clear();
  g_building_id = p.stem().stem().string();
  g_state = {};
  g_view = std::make_unique<EditorView>(
      std::make_unique<mecanvas::StbTextureProvider>(base), g_building_id);
  start_local_undo_session();
  g_error_message.clear();
  g_phase = ConnPhase::Connected;
  return true;
}

// Nothing is unpacked to disk. Ctrl+S repacks the bundle in place.
bool native_open_bundle(const std::string &path) {
  fs::path p(path);
  std::vector<unsigned char> bytes;
  if (!read_file_bytes(p, &bytes) || bytes.empty()) {
    g_error_message = "cannot read " + p.string();
    return false;
  }

  MapBundle bundle;
  try {
    bundle = imrmf::map_editor::read_bundle(bytes.data(), bytes.size());
  } catch (const std::exception &e) {
    g_error_message = std::string("not a usable map bundle: ") + e.what();
    return false;
  }

  auto blobs = std::make_shared<mecanvas::AssetBlobs>();
  for (auto &a : bundle.assets)
    (*blobs)[a.path] = std::move(a.bytes);

  g_building = std::move(bundle.building);
  g_bundle_blobs = std::move(blobs);
  g_native_bundle_path = p.string();
  g_native_yaml_path.clear();
  g_serverless_session = true;
  g_bundle_session = true;
  g_bundle_name = p.filename().string();
  g_building_id = fs::path(bundle.yaml_name).stem().stem().string();
  if (g_building_id.empty())
    g_building_id = p.stem().string();
  g_state = {};

  auto provider = std::make_unique<mecanvas::StbTextureProvider>();
  provider->set_blobs(g_bundle_blobs);
  g_view = std::make_unique<EditorView>(std::move(provider), g_building_id);
  start_local_undo_session();

  g_error_message.clear();
  if (!bundle.missing.empty()) {
    g_state.status_message = std::to_string(bundle.missing.size()) +
                             " image(s) missing from the bundle";
  }
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

bool native_create_bundle(const std::string &path) {
  std::error_code ec;
  fs::path p(path);
  if (fs::is_directory(p, ec))
    p /= p.filename().string() + imrmf::map_editor::kBundleExtension;
  if (!looks_like_map_bundle(p))
    p += imrmf::map_editor::kBundleExtension;
  if (fs::exists(p, ec) && !has_native_file_picker()) {
    g_error_message = p.string() + " already exists";
    return false;
  }

  const std::string id = p.stem().string();
  MapBundle bundle;
  bundle.yaml_name = id + ".building.yaml";
  try {
    bundle.building =
        imrmf::map_editor::parse_building(starter_building_yaml(id));
    const std::vector<unsigned char> bytes =
        imrmf::map_editor::write_bundle(bundle);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
      g_error_message = "cannot create " + p.string();
      return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              (std::streamsize)bytes.size());
  } catch (const std::exception &e) {
    g_error_message = std::string("create failed: ") + e.what();
    return false;
  }
  return native_open_bundle(p.string());
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

// Pushes a map the user picked instead of the starter map: canonical yaml
// first, then every image the bundle carried.
void native_create_building_from_file(const std::string &id,
                                      const std::string &path) {
  if (!building_id_is_valid(id)) {
    g_error_message = "name must be letters, digits, - or _ (max 64)";
    return;
  }
  if (std::find(g_buildings.begin(), g_buildings.end(), id) !=
      g_buildings.end()) {
    g_error_message = "\"" + id + "\" already exists on this backend";
    return;
  }

  fs::path p(path);
  std::error_code ec;
  if (path.empty() || !fs::is_regular_file(p, ec)) {
    g_error_message = "pick a building.yaml or a map bundle first";
    return;
  }
  std::vector<unsigned char> bytes;
  if (!read_file_bytes(p, &bytes)) {
    g_error_message = "cannot read " + path;
    return;
  }

  const bool want_bundle = g_create_format_idx == 1;
  if (want_bundle != looks_like_map_bundle(p)) {
    g_error_message = want_bundle
                          ? "that is not a .rmfmap, switch Format to "
                            "building.yaml"
                          : "that is a .rmfmap, switch Format to Full map";
    return;
  }
  const MapSource src = read_map_source(bytes, want_bundle);
  if (!src.ok) {
    g_error_message = src.error;
    return;
  }

  g_phase = ConnPhase::Loading;
  std::string payload;
  if (!take_client_result(imrmf_client_put_building(g_server_url.c_str(),
                                                    id.c_str(),
                                                    src.yaml.c_str()),
                          &payload)) {
    g_error_message = "create failed: " + payload;
    g_phase = ConnPhase::Error;
    return;
  }

  int uploaded = 0, failed = 0;
  for (const imrmf::map_editor::BundleAsset &a : src.assets) {
    if (a.bytes.empty())
      continue;
    std::string asset_payload;
    if (take_client_result(imrmf_client_put_asset(
                               g_server_url.c_str(), id.c_str(), a.path.c_str(),
                               a.bytes.data(), a.bytes.size()),
                           &asset_payload)) {
      ++uploaded;
    } else {
      ++failed;
    }
  }

  g_building_id = id;
  g_create_status.clear();
  g_state.status_message =
      failed ? "created " + id + ", " + std::to_string(failed) +
                   " image(s) failed to upload"
             : (uploaded ? "created " + id + " with " +
                               std::to_string(uploaded) + " image(s)"
                         : "created " + id);
  start_load_building();
}

bool native_write_bundle(const std::string &path) {
  if (!g_view) {
    g_error_message = "nothing to save";
    return false;
  }
  mecanvas::TextureProvider *provider = g_view->texture_provider();
  MapBundle bundle = pack_current_bundle(
      [provider](const std::string &p, std::vector<unsigned char> *out) {
        return provider && provider->read_asset(p, out);
      });

  std::vector<unsigned char> bytes;
  try {
    bytes = imrmf::map_editor::write_bundle(bundle);
  } catch (const std::exception &e) {
    g_error_message = std::string("pack failed: ") + e.what();
    return false;
  }

  fs::path p(path);
  if (!looks_like_map_bundle(p))
    p += imrmf::map_editor::kBundleExtension;

  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    g_error_message = "cannot write " + p.string();
    return false;
  }
  out.write(reinterpret_cast<const char *>(bytes.data()),
            (std::streamsize)bytes.size());
  if (!out) {
    g_error_message = "write failed: " + p.string();
    return false;
  }
  out.close();

  g_state.status_message = bundle.missing.empty()
                               ? "saved " + p.filename().string()
                               : "saved " + p.filename().string() + " — " +
                                     std::to_string(bundle.missing.size()) +
                                     " image(s) missing";
  g_error_message.clear();
  return true;
}

void reset_for_relaunch() {
  commit_pending_edits();
  g_view.reset();
  g_building = {};
  g_snapshot_building = {};
  g_state = {};
  g_building_id.clear();
  g_buildings.clear();
  g_backend_mounted = false;
  g_native_yaml_path.clear();
  g_native_bundle_path.clear();
  g_bundle_blobs.reset();
  g_bundle_session = false;
  g_bundle_name.clear();
  g_serverless_session = false;
  g_error_message.clear();
  g_request_relaunch = false;
  imrmf_client_disconnect();
  g_phase = ConnPhase::Modal;
}

void native_save() {
  // Saving clears the dirty flag, so a pending edit has to be recorded first
  // or undo would rewind past it.
  commit_pending_edits();
  if (!g_native_bundle_path.empty()) {
    if (native_write_bundle(g_native_bundle_path))
      g_state.dirty = false;
    return;
  }
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

// Fallback picker, for the browser and for platforms with no OS panel. The
// target is a parameter, since the launcher and Create pick into different
// fields.
void draw_fs_browser(float height, char *target = nullptr,
                     std::size_t target_size = 0, bool accept_yaml = true,
                     bool accept_bundle = false) {
  if (!target) {
    target = g_form.local_path;
    target_size = sizeof(g_form.local_path);
    accept_bundle = g_form.local_format_idx == 1;
    accept_yaml = !accept_bundle;
  }
  if (!g_fs.requested_once) {
    g_fs.requested_once = true;
    request_fs_list("");
  }
  ImGui::InputText("Path", target, target_size);
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
    copy_into(target, target_size, g_fs.current_path);

  ImGui::Spacing();
  ImGui::BeginChild("##fs_entries", ImVec2(0, height), true);
  for (const auto &e : g_fs.entries) {
    const std::string full = join_path(g_fs.current_path, e.name);
    if (e.is_dir) {
      if (ImGui::Selectable(("[d] " + e.name).c_str(), false))
        request_fs_list(full);
      continue;
    }
    const bool openable = (accept_bundle && e.is_map_bundle) ||
                          (accept_yaml && e.is_building_yaml);
    const ImVec4 col = openable ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
#ifndef __EMSCRIPTEN__
    // Desktop opens the file itself, so let the user click one.
    if (openable) {
      ImGui::PushStyleColor(ImGuiCol_Text, col);
      if (ImGui::Selectable(("    " + e.name).c_str(), false))
        copy_into(target, target_size, full);
      ImGui::PopStyleColor();
      continue;
    }
#endif
    ImGui::TextColored(col, "    %s%s", e.name.c_str(),
                       e.is_building_yaml ? " (building.yaml)"
                       : e.is_map_bundle  ? " (full map)"
                                          : "");
  }
  ImGui::EndChild();
}

#ifdef __EMSCRIPTEN__

// JS fetches the images, the zip is built here, so the format has one writer.
std::map<std::string, std::vector<unsigned char>> g_bundle_staging;
std::vector<unsigned char> g_bundle_bytes;
std::string g_bundle_filename;
std::string g_bundle_error;

// Object URLs so the worker path decodes off the render thread. A 10 MB png
// decoded inline in wasm stalls the frame and skips the 2048 px downscale.
EM_JS(void, imrmf_js_clear_bundle_assets, (), {
  const m = (window.imrmf && window.imrmf._bundleAssets) || null;
  if (m) {
    for (const k in m)
      URL.revokeObjectURL(m[k]);
  }
  if (window.imrmf)
    window.imrmf._bundleAssets = {};
});

EM_JS(void, imrmf_js_register_bundle_asset,
      (const char *path_c, const unsigned char *data, int len), {
        if (!window.imrmf)
          window.imrmf = {};
        if (!window.imrmf._bundleAssets)
          window.imrmf._bundleAssets = {};
        const bytes = HEAPU8.slice(data, data + len);
        window.imrmf._bundleAssets[UTF8ToString(path_c)] =
            URL.createObjectURL(new Blob([bytes]));
      });

EM_JS(const char *, imrmf_js_bundle_asset_url, (const char *path_c), {
  const m = (window.imrmf && window.imrmf._bundleAssets) || {};
  return stringToNewUTF8(m[UTF8ToString(path_c)] || "");
});

// Reads a map file the user picks and hands the bytes to wasm, which validates
// them before anything is sent anywhere.
EM_JS(void, imrmf_js_pick_create_source, (int bundle), {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = bundle ? '.rmfmap,application/zip' : '.yaml,.yml';
  inp.onchange = () => {
    const f = inp.files && inp.files[0];
    if (!f)
      return;
    f.arrayBuffer().then(buf => {
      const bytes = new Uint8Array(buf);
      const ptr = _malloc(Math.max(1, bytes.length));
      HEAPU8.set(bytes, ptr);
      const namePtr = stringToNewUTF8(f.name || 'map.yaml');
      Module._imrmf_create_source_set(ptr, bytes.length, namePtr);
      _free(namePtr);
      _free(ptr);
    });
  };
  inp.click();
});

// Staged here rather than passed per call, so the PUTs happen in one pass and
// report a single result the poll loop understands.
EM_JS(void, imrmf_js_stage_asset,
      (const char *path_c, const unsigned char *data, int len), {
        if (!window.imrmf)
          return;
        if (!window.imrmf._createAssets)
          window.imrmf._createAssets = [];
        window.imrmf._createAssets.push(
            {path : UTF8ToString(path_c), bytes : HEAPU8.slice(data, data + len)});
      });

EM_JS(void, imrmf_js_create_building,
      (const char *server_c, const char *id_c, const char *yaml_c), {
        if (!window.imrmf)
          return;
        const server = UTF8ToString(server_c);
        const id = UTF8ToString(id_c);
        const yaml = UTF8ToString(yaml_c);
        const assets = window.imrmf._createAssets || [];
        window.imrmf._createAssets = [];
        window.imrmf._result = {code : 'busy', payload : null};
        (async () => {
          try {
            const r = await fetch(server + '/buildings/' + encodeURIComponent(id),
                                  {
                                    method : 'PUT',
                                    headers : {'content-type' : 'application/yaml'},
                                    body : yaml,
                                  });
            if (!r.ok)
              throw new Error((await r.text()) || ('status ' + r.status));
            let failed = 0;
            for (const a of assets) {
              const url = server + '/layer_asset?id=' + encodeURIComponent(id) +
                          '&path=' + encodeURIComponent(a.path);
              const ar = await fetch(url, {
                method : 'PUT',
                headers : {'content-type' : 'application/octet-stream'},
                body : a.bytes,
              });
              if (!ar.ok)
                ++failed;
            }
            window.imrmf._result = {code : 'ok', payload : null};
            if (failed)
              console.warn('[imrmf] ' + failed + ' image(s) failed to upload');
          } catch (e) {
            window.imrmf._result = {code : 'err', payload : String(e)};
          }
        })();
      });

// Hands a locally picked .rmfmap straight to wasm. No server involved.
EM_JS(void, imrmf_js_open_map_bundle, (), {
  const inp = document.createElement('input');
  inp.type = 'file';
  inp.accept = '.rmfmap,application/zip';
  inp.onchange = () => {
    const f = inp.files && inp.files[0];
    if (!f)
      return;
    f.arrayBuffer().then(buf => {
      const bytes = new Uint8Array(buf);
      const ptr = _malloc(Math.max(1, bytes.length));
      HEAPU8.set(bytes, ptr);
      const namePtr = stringToNewUTF8(f.name || 'map.rmfmap');
      Module._imrmf_bundle_open(ptr, bytes.length, namePtr);
      _free(namePtr);
      _free(ptr);
    });
  };
  inp.click();
});

// Paths arrive newline-joined, which the yaml format's filenames never contain.
EM_JS(void, imrmf_js_download_map,
      (const char *server_c, const char *id_c, const char *paths_c,
       const char *name_c), {
        const id = UTF8ToString(id_c);
        const name = UTF8ToString(name_c);
        const raw = UTF8ToString(paths_c);
        const paths = raw ? raw.split('\n').filter(p => p.length) : [];
        let base = UTF8ToString(server_c) || window.location.origin || "";
        while (base.length && base[base.length - 1] === '/')
          base = base.slice(0, -1);

        const feed = (path, buf) => {
          const bytes = new Uint8Array(buf);
          const pathPtr = stringToNewUTF8(path);
          const dataPtr = _malloc(Math.max(1, bytes.length));
          HEAPU8.set(bytes, dataPtr);
          Module._imrmf_bundle_add_asset(pathPtr, dataPtr, bytes.length);
          _free(dataPtr);
          _free(pathPtr);
        };

        Promise
            .all(paths.map(
                p => fetch(base + "/layer_asset?id=" + encodeURIComponent(id) +
                           "&path=" + encodeURIComponent(p))
                         .then(r => r.ok ? r.arrayBuffer() : null)
                         // A missing image is reported, not fatal.
                         .then(buf => { if (buf) feed(p, buf); })
                         .catch(e => {
                           console.warn('[imrmf] asset fetch failed:', p, e);
                         })))
            .then(() => {
              const len = Module._imrmf_bundle_build();
              if (len <= 0) {
                console.error('[imrmf] bundle build failed');
                Module._imrmf_bundle_release();
                return;
              }
              const ptr = Module._imrmf_bundle_data();
              const blob = new Blob([ HEAPU8.slice(ptr, ptr + len) ],
                                    {type : 'application/zip'});
              const url = URL.createObjectURL(blob);
              const a = document.createElement('a');
              a.href = url;
              a.download = name;
              document.body.appendChild(a);
              a.click();
              a.remove();
              setTimeout(() => URL.revokeObjectURL(url), 10000);
              Module._imrmf_bundle_release();
            });
      });

void start_browser_download() {
  g_bundle_staging.clear();
  g_bundle_bytes.clear();
  g_bundle_error.clear();
  g_bundle_filename =
      (g_building_id.empty() ? std::string("map") : g_building_id) +
      imrmf::map_editor::kBundleExtension;

  // A bundle already holds its images and may have no server to ask.
  mecanvas::TextureProvider *provider =
      g_view ? g_view->texture_provider() : nullptr;
  std::string joined;
  for (const std::string &p : current_asset_paths()) {
    // The original bytes. The provider only keeps downscaled textures.
    if (g_bundle_blobs) {
      auto it = g_bundle_blobs->find(p);
      if (it != g_bundle_blobs->end() && !it->second.empty()) {
        g_bundle_staging[p] = it->second;
        continue;
      }
    }
    std::vector<unsigned char> bytes;
    if (provider && provider->read_asset(p, &bytes) && !bytes.empty()) {
      g_bundle_staging[p] = std::move(bytes);
      continue;
    }
    if (!joined.empty())
      joined += '\n';
    joined += p;
  }
  g_state.status_message = "packing map...";
  imrmf_js_download_map(g_server_url.c_str(), g_building_id.c_str(),
                        joined.c_str(), g_bundle_filename.c_str());
}

#endif // __EMSCRIPTEN__

#ifndef __EMSCRIPTEN__
bool g_save_bundle_modal = false;
char g_save_bundle_name[256] = "";

// Linux has no OS save panel, so Download needs somewhere to put the file.
void draw_save_bundle_modal() {
  if (g_save_bundle_modal) {
    ImGui::OpenPopup("Save map as##save_bundle");
    g_save_bundle_modal = false;
    if (g_fs.current_path.empty())
      request_fs_list("");
  }
  if (!ImGuiWidgets::BeginModal("Save map as##save_bundle", 520.0f))
    return;

  ImGui::TextDisabled("Pick a folder, then name the file. An absolute path in "
                      "the name field is used as-is.");
  ImGui::Spacing();
  ImGui::Text("Folder:");
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
  if (!g_fs.error.empty())
    ImGuiWidgets::StatusLine(theme::Signal::danger, g_fs.error.c_str());

  ImGui::BeginChild("##save_dirs", ImVec2(0, 200.0f), true);
  for (const auto &e : g_fs.entries) {
    if (e.is_dir) {
      if (ImGui::Selectable(("[d] " + e.name).c_str(), false))
        request_fs_list(join_path(g_fs.current_path, e.name));
    } else if (e.is_map_bundle) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
      if (ImGui::Selectable(("    " + e.name).c_str(), false))
        set_field(g_save_bundle_name, e.name);
      ImGui::PopStyleColor();
    }
  }
  ImGui::EndChild();

  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputText("##save_name", g_save_bundle_name,
                   sizeof(g_save_bundle_name));

  const std::string target =
      fs::path(g_save_bundle_name).is_absolute()
          ? std::string(g_save_bundle_name)
          : join_path(g_fs.current_path, g_save_bundle_name);
  std::error_code ec;
  if (g_save_bundle_name[0] && fs::exists(fs::path(target), ec))
    ImGuiWidgets::StatusLine(theme::Signal::warning,
                             "replaces an existing file");

  const int a = ImGuiWidgets::ModalActions("Save", "Cancel",
                                           g_save_bundle_name[0] != '\0');
  if (a == 1) {
    native_write_bundle(target);
    ImGui::CloseCurrentPopup();
  } else if (a == 2) {
    ImGui::CloseCurrentPopup();
  }
  ImGuiWidgets::EndModal();
}

void request_download_map() {
  const std::string suggested =
      (g_building_id.empty() ? std::string("map") : g_building_id) +
      imrmf::map_editor::kBundleExtension;
  if (has_native_file_picker()) {
    const std::string path = pick_save_path(FileKind::MapBundle, suggested);
    if (!path.empty())
      native_write_bundle(path);
    return;
  }
  set_field(g_save_bundle_name, suggested);
  g_save_bundle_modal = true;
}
#endif // !__EMSCRIPTEN__

#ifdef __EMSCRIPTEN__
// Validates the picked file in wasm, then hands the canonical yaml and the
// images to JS to PUT. The poll loop takes it from there.
void start_create_from_source(const std::string &id) {
  if (!building_id_is_valid(id)) {
    g_error_message = "name must be letters, digits, - or _ (max 64)";
    return;
  }
  if (std::find(g_buildings.begin(), g_buildings.end(), id) !=
      g_buildings.end()) {
    g_error_message = "\"" + id + "\" already exists on this backend";
    return;
  }
  const std::string name = g_create_source;
  const bool looks_bundle =
      name.size() > 7 && name.compare(name.size() - 7, 7, ".rmfmap") == 0;
  const bool want_bundle = g_create_format_idx == 1;
  if (want_bundle != looks_bundle) {
    g_error_message = want_bundle
                          ? "that is not a .rmfmap, switch Format to "
                            "building.yaml"
                          : "that is a .rmfmap, switch Format to Full map";
    return;
  }
  const MapSource src = read_map_source(g_create_bytes, want_bundle);
  if (!src.ok) {
    g_error_message = src.error;
    return;
  }
  for (const imrmf::map_editor::BundleAsset &a : src.assets) {
    if (!a.bytes.empty())
      imrmf_js_stage_asset(a.path.c_str(), a.bytes.data(), (int)a.bytes.size());
  }
  g_building_id = id;
  g_phase = ConnPhase::Loading;
  g_pending_create = true;
  imrmf_reset_result();
  imrmf_js_create_building(g_server_url.c_str(), id.c_str(), src.yaml.c_str());
}
#endif

// Where the new building's contents come from. The OS panel on macOS, the
// in-app browser elsewhere, and a file input in the browser.
void draw_create_source_picker() {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped(
      g_create_format_idx == 1
          ? "The bundle is read here, checked, and written to the backend "
            "under "
            "the new name, images and all."
          : "The building.yaml is read here, checked, and written to the "
            "backend under the new name. Its images are not uploaded, a bundle "
            "carries those.");
  ImGui::PopStyleColor();
  ImGui::Spacing();

  const bool want_bundle = g_create_format_idx == 1;
#ifdef __EMSCRIPTEN__
  if (ImGui::Button(want_bundle ? ICON_MDI_FOLDER_OPEN " Choose a .rmfmap"
                                : ICON_MDI_FOLDER_OPEN
                        " Choose a building.yaml"))
    imrmf_js_pick_create_source(want_bundle ? 1 : 0);
  if (g_create_source[0]) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", g_create_source);
  }
#else
  if (has_native_file_picker()) {
    if (ImGui::Button(want_bundle ? ICON_MDI_FOLDER_OPEN " Choose a .rmfmap"
                                  : ICON_MDI_FOLDER_OPEN
                          " Choose a building.yaml")) {
      const std::string picked = pick_open_path(
          want_bundle ? FileKind::MapBundle : FileKind::BuildingYaml);
      if (!picked.empty()) {
        set_field(g_create_source, picked);
        g_error_message.clear();
      }
    }
    if (g_create_source[0])
      ImGui::TextDisabled("%s", g_create_source);
  } else {
    const std::string before = g_create_source;
    draw_fs_browser(160.0f, g_create_source, sizeof(g_create_source),
                    /*accept_yaml=*/!want_bundle,
                    /*accept_bundle=*/want_bundle);
    if (before != g_create_source)
      g_error_message.clear();
  }
#endif
  if (!g_create_status.empty())
    ImGuiWidgets::StatusLine(theme::Signal::warning, g_create_status.c_str());
}

// One dialog for both front ends. Only the OS file picker differs.
void draw_open_dialog_body() {
  const bool mounted = g_backend_mounted;
  const bool local = g_form.kind_idx == 0;
  ImGui::TextDisabled(g_locked ? "This server serves one fixed backend."
                               : "Choose where your building.yaml lives.");
  ImGuiWidgets::SectionGap();
#ifdef __EMSCRIPTEN__
  const bool os_picker = false;
#else
  const bool os_picker = local && has_native_file_picker();
#endif

  // A locked server rejects /mount, so the backend form would only error.
#ifdef __EMSCRIPTEN__
  if (g_locked && !mounted) {
    ImGui::TextWrapped(
        "The backend was chosen when the server started%s%s. You can reconnect "
        "to it, or open a map file instead.",
        g_state.branch.empty() ? "" : " on branch ",
        g_state.branch.empty() ? "" : g_state.branch.c_str());
#ifdef __EMSCRIPTEN__
    ImGuiWidgets::SectionGap();
    if (ImGui::Button(ICON_MDI_FOLDER_OPEN " Open a .rmfmap file"))
      imrmf_js_open_map_bundle();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "Picks from the machine running this browser, not from the server. "
        "The map is then held in memory and Download map is the only way to "
        "keep changes.");
    ImGui::PopStyleColor();
#endif
    if (!g_error_message.empty()) {
      ImGui::Spacing();
      ImGuiWidgets::StatusLine(theme::Signal::danger, g_error_message.c_str());
    }
    if (ImGuiWidgets::ModalActions("Reconnect") == 1)
      start_list_buildings();
    return;
  }
#endif

  if (!g_locked) {
    // Form table, so long labels cannot widen the dialog.
    if (ImGuiWidgets::BeginFormTable("##open_form")) {
      // The browser needs a server even for a local file, since it does the
      // read.
#ifdef __EMSCRIPTEN__
      const bool needs_server_url = true;
#else
      const bool needs_server_url = !local;
#endif
      if (needs_server_url) {
        ImGuiWidgets::FormRow("Server URL");
        ImGui::SetNextItemWidth(-FLT_MIN); // fills the cell
        ImGui::InputText("##server_url", g_form.server_url,
                         sizeof(g_form.server_url));
#ifndef __EMSCRIPTEN__
        if (ImGui::IsItemDeactivatedAfterEdit())
          native_probe_server_config();
#endif
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
#ifndef __EMSCRIPTEN__
        if (g_form.kind_idx == 1)
          native_probe_server_config();
#endif
      }

      if (local) {
        ImGuiWidgets::FormRow("Format");
        const int fmt = ImGuiWidgets::ButtonGroupSelector(
            {"building.yaml", "Full map (.rmfmap)"}, g_form.local_format_idx,
            ImVec2(0, 0));
        if (fmt >= 0 && fmt != g_form.local_format_idx) {
          g_form.local_format_idx = fmt;
          g_error_message.clear();
        }
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
          const bool is_secret = (f.buffer == g_form.s3_secret);
          const bool is_access = (f.buffer == g_form.s3_access);
          bool *held = is_secret   ? &g_form.s3_secret_is_server_held
                       : is_access ? &g_form.s3_access_is_server_held
                                   : nullptr;
          // The mask is already dots, no need to hide dots behind asterisks.
          ImGuiInputTextFlags flags = f.flags;
          if (held && *held)
            flags &= ~ImGuiInputTextFlags_Password;
          ImGui::InputText(f.id, f.buffer, f.size, flags);
          // Clear on focus, so a replacement is not appended to the mask.
          if (held && *held && ImGui::IsItemActivated()) {
            *held = false;
            f.buffer[0] = '\0';
          }
        }
        if (g_server_has_s3_secret || g_server_has_s3_access) {
          ImGuiWidgets::FormRow("");
          ImGui::PushStyleColor(
              ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
          ImGui::TextWrapped(
              g_form.s3_secret_is_server_held
                  ? "Credentials stay on the server. Leave them masked to "
                    "reuse them, or type to replace them."
                  : "Leave a credential empty to reuse the server's.");
          ImGui::PopStyleColor();
        }
      }

      ImGui::EndTable();
    }

    if (local) {
      ImGuiWidgets::SectionGap();
      const bool bundle_fmt = g_form.local_format_idx == 1;
#ifdef __EMSCRIPTEN__
      if (bundle_fmt) {
        ImGui::TextWrapped(
            "Picks a .rmfmap from the machine running this browser, not from "
            "the server. It is edited in memory: no collaboration and no "
            "autosave, and Download map is how you keep your changes.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_MDI_FOLDER_OPEN " Choose a .rmfmap file"))
          imrmf_js_open_map_bundle();
        if (!g_error_message.empty()) {
          ImGui::Spacing();
          ImGuiWidgets::StatusLine(theme::Signal::danger,
                                   g_error_message.c_str());
        }
        return;
      }
#endif
      if (os_picker) {
        ImGui::TextWrapped(
            bundle_fmt
                ? "Open a .rmfmap map bundle. It stays in memory — the images "
                  "are never unpacked to disk, and saving rewrites the bundle."
                : "Open a building.yaml, or create one. Edits are written "
                  "straight back to the file.");
        if (g_form.local_path[0])
          ImGui::TextDisabled("%s", g_form.local_path);
      } else {
        draw_fs_browser(220.0f);
      }
    } else if (!mounted) {
      ImGui::TextDisabled("Leave the endpoint empty for AWS, set it for an "
                          "S3-compatible service.");
    }
  }

  if (mounted) {
    ImGuiWidgets::SectionGap();

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::palette::success, ICON_MDI_CIRCLE_MEDIUM);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::success, "Connected");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", connection_summary().c_str());

    if (!g_locked) {
      const float w = ImGui::CalcTextSize(ICON_MDI_CLOSE).x +
                      ImGui::GetStyle().FramePadding.x * 2.0f;
      ImGui::SameLine();
      const float x = ImGui::GetContentRegionMax().x - w;
      if (x > ImGui::GetCursorPosX())
        ImGui::SetCursorPosX(x);
      const bool drop = ImGui::SmallButton(ICON_MDI_CLOSE "##drop_backend");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Disconnect and choose a different backend");
      if (drop) {
#ifdef __EMSCRIPTEN__
        disconnect_and_reset();
#else
        g_buildings.clear();
        g_backend_mounted = false;
        g_error_message.clear();
        g_phase = ConnPhase::Modal;
#endif
        return;
      }
    }

    ImGuiWidgets::SectionGap();

    // With nothing to open, the only action left is to make one. A locked
    // server still takes new buildings, the lock is only about remounting.
    const bool has_any = !g_buildings.empty();
    if (!has_any)
      g_building_mode = 1;

    if (ImGuiWidgets::BeginFormTable("##open_building")) {
      if (has_any) {
        ImGuiWidgets::FormRow("Building");
        const int m = ImGuiWidgets::ButtonGroupSelector(
            {"Open existing", "Create new"}, g_building_mode, ImVec2(0, 0));
        if (m >= 0 && m != g_building_mode) {
          g_building_mode = m;
          g_error_message.clear();
        }
      }

      if (g_building_mode == 0) {
        ImGuiWidgets::FormRow(has_any ? "Open" : "Building");
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
        if (ImGui::Combo("##building", &selected, names.data(),
                         (int)names.size()))
          g_building_id = g_buildings[selected];
      } else {
        ImGuiWidgets::FormRow("New name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Editing anything here invalidates whatever the last attempt said.
        if (ImGui::InputText("##new_name", g_form.building_id,
                             sizeof(g_form.building_id)))
          g_error_message.clear();
        if (g_form.building_id[0] &&
            !building_id_is_valid(g_form.building_id)) {
          ImGuiWidgets::FormRow("");
          ImGui::TextColored(theme::palette::danger,
                             "letters, digits, - and _ only");
        }
        ImGuiWidgets::FormRow("Contents");
        if (ImGui::Checkbox("Start from a map file I already have",
                            &g_create_from_file))
          g_error_message.clear();
        if (g_create_from_file) {
          ImGuiWidgets::FormRow("Format");
          const int fmt = ImGuiWidgets::ButtonGroupSelector(
              {"building.yaml", "Full map (.rmfmap)"}, g_create_format_idx,
              ImVec2(0, 0));
          if (fmt >= 0 && fmt != g_create_format_idx) {
            g_create_format_idx = fmt;
            // The picked file belongs to the format it was picked under.
            g_create_source[0] = '\0';
#ifdef __EMSCRIPTEN__
            g_create_bytes.clear();
#endif
            g_error_message.clear();
          }
        }
      }
      ImGui::EndTable();
    }

    if (g_building_mode == 1 && g_create_from_file)
      draw_create_source_picker();

    if (!has_any) {
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("This backend has no buildings yet, so name one to "
                         "start from an empty map.");
      ImGui::PopStyleColor();
    }
  }

  if (!g_error_message.empty()) {
    ImGui::Spacing();
    ImGuiWidgets::StatusLine(theme::Signal::danger, g_error_message.c_str());
  }

  // Desktop reads local files itself, the browser goes through the server.
#ifdef __EMSCRIPTEN__
  const bool opens_a_file = os_picker;
#else
  const bool opens_a_file = local;
#endif
  const bool creating = mounted && g_building_mode == 1;
  const char *primary = creating                    ? "Create"
                        : (opens_a_file || mounted) ? "Open"
                                                    : "Connect";
  // No mounted list on desktop, and the file may not exist yet.
  const char *secondary =
      (opens_a_file && !mounted && !g_locked) ? "Create" : nullptr;

  bool ready = true;
  if (creating) {
    ready = building_id_is_valid(g_form.building_id) &&
            (!g_create_from_file || g_create_source[0] != '\0');
  } else if (!g_locked && !mounted) {
    ready = os_picker ||
            (local ? g_form.local_path[0] != '\0'
                   : g_form.s3_bucket[0] && g_form.s3_region[0] &&
                         (g_form.s3_access[0] || g_server_has_s3_access) &&
                         (g_form.s3_secret[0] || g_server_has_s3_secret));
  }

  switch (ImGuiWidgets::ModalActions(primary, secondary, ready)) {
  case 1:
#ifndef __EMSCRIPTEN__
    // Only when no server is in play. A server whose backend is local still
    // owns the map, and opening its root as a file would bypass the CRDT.
    if (local && !mounted) {
      const bool bundle = g_form.local_format_idx == 1;
      std::string path = g_form.local_path;
      if (has_native_file_picker()) {
        path = pick_open_path(bundle ? FileKind::MapBundle
                                     : FileKind::BuildingYaml);
        if (path.empty())
          return;
        set_field(g_form.local_path, path);
      }
      if (path.empty())
        return;
      if (bundle)
        native_open_bundle(path);
      else
        native_open_local(path);
      return;
    }
#endif
    if (mounted) {
      if (g_building_mode == 1) {
        if (!building_id_is_valid(g_form.building_id)) {
          g_error_message = "give the new building a name of letters, digits, "
                            "- or _";
        } else if (g_create_from_file) {
#ifdef __EMSCRIPTEN__
          start_create_from_source(g_form.building_id);
#else
          native_create_building_from_file(g_form.building_id, g_create_source);
#endif
        } else {
          start_create_building(g_form.building_id);
        }
      } else {
        start_load_building();
      }
    } else {
      start_mount();
    }
    return;
  case 2:
#ifndef __EMSCRIPTEN__
    if (local && !mounted) {
      const bool bundle = g_form.local_format_idx == 1;
      std::string path = g_form.local_path;
      if (has_native_file_picker()) {
        path = pick_save_path(
            bundle ? FileKind::MapBundle : FileKind::BuildingYaml,
            bundle
                ? std::string("untitled") + imrmf::map_editor::kBundleExtension
                : std::string("untitled.building.yaml"));
        if (path.empty())
          return;
        set_field(g_form.local_path, path);
      }
      if (path.empty())
        g_error_message = "choose where the new file should go";
      else if (bundle)
        native_create_bundle(path);
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
#ifdef __EMSCRIPTEN__
void draw_connection_modal() {
  if (!ImGui::IsPopupOpen("Open a building##imrmf"))
    ImGui::OpenPopup("Open a building##imrmf");
  // Not dismissable, there is no app behind this until a building is chosen.
  if (ImGuiWidgets::BeginModal("Open a building##imrmf", 560.0f,
                               /*fill_host=*/false,
                               /*dismissable=*/false)) {
    draw_open_dialog_body();
    ImGuiWidgets::EndModal();
  }
}
#endif

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
  if (g_bundle_session) {
    summary = "Map file";
    if (!g_bundle_name.empty()) {
      summary += " \xC2\xB7 ";
      summary += g_bundle_name;
    }
  } else if (g_form.kind_idx == 0) {
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
    if (v.empty())
      return;
    details += k;
    details += ": ";
    details += v;
    details += "\n";
    h.detail_rows.emplace_back(k, v);
  };
  add("Building", g_building_id);
  if (g_bundle_session) {
    add("Source",
        g_bundle_name.empty() ? std::string("a .rmfmap file") : g_bundle_name);
#ifdef __EMSCRIPTEN__
    add("Backend", "None, the map is held in this tab");
    add("Saving",
        "Download map writes a new .rmfmap, the original is untouched");
#else
    add("Backend", g_native_bundle_path);
    add("Saving", "Ctrl/Cmd+S or the save button repacks that file in place");
#endif
  } else if (g_form.kind_idx == 0) {
    // A server can be mounted on a local directory. That is still the server's
    // map, not a file this app writes.
    add("Backend", g_serverless_session ? "Local file"
                                        : "Local files, through the server");
    add("Path", g_form.local_path);
    if (!g_serverless_session)
      add("Server", g_server_url);
    add("Saving",
        !g_serverless_session
            ? "autosaves through the server, edits merge across clients"
            : (g_native_bundle_path.empty()
                   ? "writes the building.yaml in place on Ctrl/Cmd+S"
                   : "repacks the .rmfmap bundle in place on Ctrl/Cmd+S"));
  } else {
    add("Backend", "S3 bucket");
    add("Bucket", g_form.s3_bucket);
    add("Prefix", g_form.s3_prefix);
    add("Region", g_form.s3_region);
    add("Endpoint", g_form.s3_endpoint);
    add("Server", g_server_url);
    add("Saving", "autosaves through the server, edits merge across clients");
  }
  add("Branch", g_state.branch);
  if (!details.empty() && details.back() == '\n')
    details.pop_back();
  h.details = std::move(details);

  h.can_disconnect = !g_locked;
#ifdef __EMSCRIPTEN__
  h.on_disconnect = []() { disconnect_and_reset(); };
#else
  // Desktop picks its map in the launcher, so there is nowhere to go back to.
  h.on_disconnect = nullptr;
#endif
#ifdef __EMSCRIPTEN__
  h.on_download_map = []() { start_browser_download(); };
#else
  h.on_download_map = []() { request_download_map(); };
#endif
  h.has_server = !g_serverless_session;
  h.dirty = g_state.dirty;
  h.on_flush_edits = []() { commit_pending_edits(); };
#ifndef __EMSCRIPTEN__
  if (g_serverless_session) {
    h.can_undo = []() { return g_history.can_undo(); };
    h.can_redo = []() { return g_history.can_redo(); };
    h.on_undo = []() { history_undo(); };
    h.on_redo = []() { history_redo(); };
  }
#endif
#ifndef __EMSCRIPTEN__
  // Only a session backed by a real file can be re-saved to it.
  if (!g_native_bundle_path.empty() || !g_native_yaml_path.empty())
    h.on_save_in_place = []() { native_save(); };
  h.disconnect_label = g_serverless_session ? "Close file" : "Disconnect";
  h.on_disconnect = []() { g_request_relaunch = true; };
#endif
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
    // A file session has no document. Undo is in memory, and there is no peer
    // to mirror from.
    const bool has_doc = !g_serverless_session;
    if (has_doc)
      mirror_from_yjs();
    // Snapshots and branches are the server's, and there is none.
    if (!g_serverless_session) {
      issue_snapshot_requests();
      issue_branch_requests();
    }
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
      draw_busy("waiting for initial sync...");
    }
#ifndef __EMSCRIPTEN__
    draw_save_bundle_modal();
#endif
    ImGui::End();
    if (g_state.snapshot_dir.empty())
      commit_pending_edits_if_settled();
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

// The browser page lives as long as the tab, so only the desktop tears down.
#ifndef __EMSCRIPTEN__
void shutdown_backends() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
}
#endif

#ifndef __EMSCRIPTEN__

// Nudges a window inside the work area, keeping its position otherwise.
void keep_on_screen(GLFWwindow *win, int w, int h) {
  int mx = 0, my = 0, mw = 0, mh = 0;
  glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
  if (mw <= 0 || mh <= 0)
    return;
  int x = 0, y = 0;
  glfwGetWindowPos(win, &x, &y);
  const int nx = std::max(mx, std::min(x, mx + std::max(0, mw - w)));
  const int ny = std::max(my, std::min(y, my + std::max(0, mh - h)));
  if (nx != x || ny != y)
    glfwSetWindowPos(win, nx, ny);
}

void center_window(GLFWwindow *win, int w, int h) {
  int mx = 0, my = 0, mw = 0, mh = 0;
  glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &mx, &my, &mw, &mh);
  if (mw > 0 && mh > 0)
    glfwSetWindowPos(win, mx + (mw - w) / 2, my + (mh - h) / 2);
}

// Runs before the editor window. The window follows the modal's size.
bool run_launcher() {
  // No page origin to infer from on desktop. IMRMF_SERVER_URL matches how the
  // container is configured, otherwise fall back to the documented port.
  if (g_form.server_url[0] == '\0') {
    const char *env = std::getenv("IMRMF_SERVER_URL");
    set_field(g_form.server_url,
              std::string(env && env[0] ? env : "http://localhost:30010"));
  }
  native_probe_server_config();
  // A locked server has one backend, so go straight to its building list.
  if (g_locked)
    start_list_buildings();

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
  int unsettled = 0;
  int tallest = 0;
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

    // A fresh popup reports a placeholder size and resizing to it sticks, so
    // wait for two frames to agree. If they never do (height depends on the
    // width given), take the tallest seen rather than leaving it clipped.
    int current_w = 0, current_h = 0;
    glfwGetWindowSize(g_window, &current_w, &current_h);
    const int height = (int)(wanted_height + 0.5f);
    auto fit = [&](int h) {
      glfwSetWindowSize(g_window, width, h);
      // Only the first fit centres, later ones may have been dragged.
      if (!centred) {
        center_window(g_window, width, h);
        centred = true;
      }
      unsettled = 0;
      tallest = 0;
    };
    if (height > 1 && height != current_h) {
      tallest = std::max(tallest, height);
      if (height == last_height)
        fit(height);
      else if (++unsettled > 30)
        fit(tallest);
    } else {
      unsettled = 0;
      tallest = 0;
    }
    last_height = height;

    // Height and work area both move under us (content appears, a remote
    // desktop resizes when clients attach), so re-check every frame. No
    // re-centring, a window the user dragged stays put.
    keep_on_screen(g_window, width, current_h);
  }

  ImGui::GetStyle().DisplaySafeAreaPadding = safe_area;
  shutdown_backends();
  glfwDestroyWindow(g_window);
  g_window = nullptr;
  return g_phase == ConnPhase::Connected;
}

#endif // __EMSCRIPTEN__

} // namespace

#ifdef __EMSCRIPTEN__
// Kept out of the anonymous namespace so the exported names survive linking.
extern "C" {

EMSCRIPTEN_KEEPALIVE void
imrmf_bundle_add_asset(const char *path, const unsigned char *data, int len) {
  if (!path || !data || len <= 0)
    return;
  g_bundle_staging[path].assign(data, data + len);
}

// Returns the packed size, 0 on failure.
EMSCRIPTEN_KEEPALIVE int imrmf_bundle_build() {
  try {
    MapBundle bundle = pack_current_bundle(
        [](const std::string &p, std::vector<unsigned char> *out) {
          auto it = g_bundle_staging.find(p);
          if (it == g_bundle_staging.end())
            return false;
          *out = it->second;
          return true;
        });
    g_bundle_bytes = imrmf::map_editor::write_bundle(bundle);
    g_state.status_message = bundle.missing.empty()
                                 ? "downloaded " + g_bundle_filename
                                 : "downloaded " + g_bundle_filename + " — " +
                                       std::to_string(bundle.missing.size()) +
                                       " image(s) missing";
  } catch (const std::exception &e) {
    g_bundle_error = e.what();
    g_state.status_message = std::string("pack failed: ") + e.what();
    g_bundle_bytes.clear();
    return 0;
  }
  return (int)g_bundle_bytes.size();
}

EMSCRIPTEN_KEEPALIVE const unsigned char *imrmf_bundle_data() {
  return g_bundle_bytes.data();
}

EMSCRIPTEN_KEEPALIVE void imrmf_bundle_release() {
  g_bundle_staging.clear();
  g_bundle_bytes.clear();
  g_bundle_bytes.shrink_to_fit();
}

// No mount and no CRDT. Download is the only way to save.
// Nothing is validated or sent until the user presses Create.
EMSCRIPTEN_KEEPALIVE void imrmf_create_source_set(const unsigned char *data,
                                                  int len, const char *name) {
  g_create_status.clear();
  g_create_bytes.clear();
  g_create_source[0] = '\0';
  if (!data || len <= 0)
    return;
  g_create_bytes.assign(data, data + len);
  if (name)
    set_field(g_create_source, std::string(name));
}

EMSCRIPTEN_KEEPALIVE int imrmf_bundle_open(const unsigned char *data, int len,
                                           const char *name) {
  if (!data || len <= 0) {
    g_error_message = "empty file";
    return 0;
  }
  MapBundle bundle;
  try {
    bundle = imrmf::map_editor::read_bundle(data, (std::size_t)len);
  } catch (const std::exception &e) {
    g_error_message = std::string("not a usable map bundle: ") + e.what();
    return 0;
  }

  auto blobs = std::make_shared<mecanvas::AssetBlobs>();
  for (auto &a : bundle.assets)
    (*blobs)[a.path] = std::move(a.bytes);

  g_building = std::move(bundle.building);
  g_bundle_blobs = std::move(blobs);
  g_building_id =
      std::string(bundle.yaml_name, 0, bundle.yaml_name.find(".building.yaml"));
  if (g_building_id.empty())
    g_building_id = "map";
  g_serverless_session = true;
  g_bundle_session = true;
  g_bundle_name = name ? name : "";
  g_state = {};

  // Object URLs so the images render through the worker and a big map does not
  // lock the tab.
  imrmf_js_clear_bundle_assets();
  for (const auto &[path, bytes] : *g_bundle_blobs) {
    if (!bytes.empty())
      imrmf_js_register_bundle_asset(path.c_str(), bytes.data(),
                                     (int)bytes.size());
  }
  auto provider = std::make_unique<mecanvas::HttpTextureProvider>(
      [](const std::string &, const std::string &path) {
        return take_string(imrmf_js_bundle_asset_url(path.c_str()));
      });
  g_view = std::make_unique<EditorView>(std::move(provider), g_building_id);

  g_error_message.clear();
  if (!bundle.missing.empty()) {
    g_state.status_message = std::to_string(bundle.missing.size()) +
                             " image(s) missing from the bundle";
  }
  g_phase = ConnPhase::Connected;
  return 1;
}

} // extern "C"
#endif // __EMSCRIPTEN__

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
  g_window = create_window(1600, 900, "RMF Map Editor", true);
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
  // Closing a map comes back to the launcher rather than quitting.
  for (;;) {
    if (!run_launcher())
      break;

    g_window = create_window(1600, 900, "RMF Map Editor", true);
    if (!g_window) {
      glfwTerminate();
      return 1;
    }
    init_backends(g_window);
    g_request_relaunch = false;
    while (!glfwWindowShouldClose(g_window) && !g_request_relaunch)
      frame();
    const bool relaunch = g_request_relaunch;
    shutdown_backends();
    glfwDestroyWindow(g_window);
    g_window = nullptr;
    if (!relaunch)
      break;
    reset_for_relaunch();
  }
#endif

  ImGui::DestroyContext();
  glfwTerminate();
  return 0;
}
