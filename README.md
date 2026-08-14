# ImRmfMapEditor

Opinionated, ImGui-based editor for OpenRMF building map files. The front-end
talks Yjs over WebSocket to a Rust server that hosts the CRDT and flushes
`building.yaml` to disk or S3. It builds two ways from the same sources: a wasm
app for the browser, and a desktop app for macOS and Linux.

## Features

- Browser and desktop from one codebase, sharing the editor, the UI kit, and
  the sync protocol.
- Local filesystem and S3 storage backends. Optional auto-mount + auto-load
  at startup.
- Real-time multi-user collaboration via CRDT. Concurrent edits merge.
- Autosync with server-side validation; revert to last valid on bad edits.
- Browser-based, no install. Canvas embeds read-only into other apps.
- Batch edit shared fields (`bidirectional`, `orientation`) across selected
  lanes with mixed-value handling.
- Lane dissection: drag across lanes to split each at the intersection,
  parameters inherited.
- Marquee selects vertices and lanes together.
- Continuous lane drawing with H / V / 45° snap.
- Context-aware delete with optional cascade to referencing lanes only.

## Shortcuts

| Key                  | Action                                        |
| -------------------- | --------------------------------------------- |
| `S` / `V` / `L`      | Select / Vertex / Lane mode                   |
| `W` / `D` / `M`      | Wall / Door / Measurement mode                |
| `F` / `H`            | Floor / Hole polygon mode                     |
| `Enter`              | Close the floor/hole polygon                  |
| `Esc`                | Break the lane/wall chain or cancel a polygon |
| `Delete`             | Delete current selection                      |
| `Ctrl/Cmd+S`         | Force flush                                   |
| `Ctrl+Z` / `Ctrl+Y`  | Undo / Redo                                   |
| `Shift` (drag)       | Snap to H / V / 45°                           |
| `Shift` (click)      | Add / toggle in selection                     |
| Middle-mouse / wheel | Pan / zoom                                    |

## Layout

```
model/        building.yaml parse/serialize. No UI / fetch deps.
canvas/       Map renderer (native + wasm). Embeddable read-only.
ui/           Shared ImGui theme, widgets, fonts, icons.
view/         EditorView: modes, selection, Yjs op wiring.
app/          Front-end: wasm (:editor_wasm) and desktop (:editor).
client_rust/  Desktop CRDT client: y-sync over WebSocket + REST, as a C ABI.
server_rust/  axum/yrs WebSocket server: CRDT + REST. :core is shared with the
              desktop client.
server/       Shared buildings_api.
third_party/  Patched rmf_building_map_tools.
```

The browser gets fetch, the WebSocket, and the Yjs provider from JS. The desktop
gets the same three from `client_rust`, which reuses the server's own protocol
and yaml-bridge code, so both speak the same wire format to the same server.

## Run

### Browser

```bash
bazel run //:dev
```

Open http://localhost:30010 and use the connect modal.

Forward server flags after `--`:

```bash
bazel run //:dev -- --port 30099 --no-validate
```

### Desktop

```bash
bazel run //app:editor
```

Opens a dialog first, then the editor window once a building is chosen. The
dialog is the same one the browser shows, with the same two backends:

- **Local file** reads and writes the `building.yaml` directly, with no server
  and no CRDT. `Ctrl/Cmd+S` writes it back. macOS uses the system open/save
  panels, elsewhere the dialog browses the filesystem itself.
- **S3 Bucket** mounts through a running server and then syncs over Yjs, so
  desktop and browser clients edit the same building together.

Snapshots and branches are browser-only for now.

Check the client against a running server without the UI:

```bash
bazel run //client_rust:smoke_test -- http://localhost:30010 <building-id>
```

### Platform notes

macOS needs the Apple CC toolchain for glfw's objc backend, which `.bazelrc`
selects under `build:macos`. The desktop app hides the system title bar there
and draws its own, keeping the window's shadow and its close/minimise/zoom
buttons. Linux keeps the system title bar and uses the in-app file browser.

## Docker

Build the artifacts, stage them where the `Dockerfile` expects them (`dist/`,
dereferencing Bazel's output symlinks), then build and run:

The image includes `building_map_server`, the ROS2 `/map` publisher the
entrypoint starts. It builds by default. macOS turns it off via `.bazelrc`
(`--define=ros2=false`) because rules_ros2 does not build there.

```bash
bazel build //server_rust:imrmf_map_editor
bazel build --config=wasm //app:editor_wasm

mkdir -p dist/server_rust dist/app
cp -RL bazel-bin/server_rust/imrmf_map_editor          dist/server_rust/
cp -RL bazel-bin/server_rust/imrmf_map_editor.runfiles dist/server_rust/
cp -RL bazel-bin/app/editor_wasm                       dist/app/

docker build -t imrmf-map-editor:dev -f Dockerfile .
docker run --rm -p 30010:30010 -v "$(pwd)/maps:/var/imrmf/cache" imrmf-map-editor:dev
```

CI builds and publishes this image to `ghcr.io/eigenblue-ai/imrmf_map_editor`
(see `.github/workflows/build-image.yml`).

## Feature gaps vs upstream traffic-editor

Comparison against [`osrf/ros2multirobotbook/traffic-editor`](https://osrf.github.io/ros2multirobotbook/traffic-editor.html). Key: ✅ done · 🟡 partial · ❌ missing · — not applicable upstream.

| Area              | Feature                                                                 | traffic-editor | imrmf_map_editor | Notes                                                                            |
| ----------------- | ----------------------------------------------------------------------- | -------------- | ---------------- | -------------------------------------------------------------------------------- |
| Geometry tools    | Walls (`w`)                                                             | ✅             | ✅               | draw chain, select, edit textures, delete                                        |
|                   | Floor polygons                                                          | ✅             | ✅               | click a loop to define; fill + outline; param edit                               |
|                   | Hole polygons                                                           | ✅             | ✅               | added to the selected floor; outline only (fill doesn't subtract yet)            |
|                   | Edit polygon tool                                                       | ✅             | 🟡               | loop vertices move with the shared vertex drag; no add/remove-vertex-in-loop yet |
|                   | Doors (hinged/sliding/etc.)                                             | ✅             | ✅               | type + motion params, swing-arc glyph                                            |
|                   | Lifts (multi-level, cabin, doors)                                       | ✅             | ❌               | `Building::passthrough`                                                          |
|                   | Fiducials                                                               | ✅             | ❌               | no inter-level auto-transform                                                    |
|                   | Measurements (scale calibration)                                        | ✅             | ✅               | create + edit `distance`; feeds level scale                                      |
|                   | Model / asset placement                                                 | ✅             | ❌               | no thumbnail library                                                             |
| Edit ops          | Move tool (`m`)                                                         | ✅             | 🟡               | only via Vertex-mode drag                                                        |
|                   | Rotate tool (`r`)                                                       | ✅             | ❌               | —                                                                                |
|                   | Toggle bidirectional (`b`)                                              | ✅             | ❌               | edit via attribute panel only                                                    |
|                   | Grid snap                                                               | ✅             | 🟡               | shift→H/V/45° only, no fixed grid                                                |
|                   | Marquee select lanes                                                    | —              | ✅               |                                                                                  |
|                   | Multi-vertex align H/V                                                  | —              | ✅               |                                                                                  |
| Vertex params     | Standard 11 RMF keys                                                    | ✅             | ✅               | match                                                                            |
|                   | `mutex`, `merge_radius`                                                 | —              | ✅               | extras                                                                           |
|                   | `Vertex.z` (elevation)                                                  | ✅             | 🟡               | in struct, not in UI                                                             |
| Lane params       | bidirectional, orientation, graph*idx, speed_limit, demo_mock*\*, mutex | ✅             | ✅               | match                                                                            |
|                   | Direction arrows                                                        | ✅             | ✅               | match                                                                            |
| Level metadata    | `elevation` editor                                                      | ✅             | ❌               | in struct, not editable                                                          |
|                   | `drawing_filename` direct edit                                          | ✅             | 🟡               | only via Layers flow                                                             |
|                   | Add / rename / reorder levels                                           | ✅             | ❌               | dropdown selector only                                                           |
| Building metadata | `name` editor                                                           | ✅             | ❌               | in struct, not editable                                                          |
|                   | `coordinate_system` editor                                              | ✅             | ❌               | inherits loaded value                                                            |
| Layer             | scale / yaw / translation                                               | ✅             | ✅               | match                                                                            |
|                   | RGB color                                                               | ✅             | ✅               | match                                                                            |
|                   | Alpha (`color_a`)                                                       | ✅             | ✅               | match                                                                            |
|                   | `visible` toggle                                                        | ✅             | ✅               | match                                                                            |
| Sidebar           | Levels panel                                                            | ✅             | ❌               | —                                                                                |
|                   | Layers panel                                                            | ✅             | ✅               | match (overlay)                                                                  |
|                   | Lifts panel                                                             | ✅             | ❌               | —                                                                                |
|                   | Traffic tab + per-graph visibility (0..8)                               | ✅             | ❌               | all lanes always drawn                                                           |
|                   | Live cursor coords                                                      | ✅             | ❌               | —                                                                                |
|                   | Graph color legend                                                      | ✅             | ❌               | —                                                                                |
| Crowd sim         | `human_goal_set_name` on vertex                                         | ✅             | ✅               | match                                                                            |
|                   | `crowd_sim` block editor (agent_groups, profiles, transitions)          | ✅             | ❌               | `Building::passthrough`                                                          |
| Workflow          | `building_map_generator` integration                                    | ✅             | 🟡               | server validates only, no UI button                                              |
|                   | Nav-graph (`*.nav.yaml`) export                                         | ✅             | ❌               | —                                                                                |
|                   | 3D / wall-mesh preview / export                                         | ✅             | ❌               | —                                                                                |
|                   | Scenario / task authoring                                               | ✅             | ❌               | —                                                                                |
| Sync / storage    | Local filesystem                                                        | ✅             | ✅               | match                                                                            |
|                   | S3 backend                                                              | —              | ✅               |                                                                                  |
|                   | Multi-user Yjs CRDT                                                     | —              | ✅               |                                                                                  |
|                   | Browser / wasm UI                                                       | —              | ✅               |                                                                                  |
|                   | Desktop app (macOS / Linux)                                             | ✅             | ✅               | same dialog and editor as the browser; local files sync-free, S3 over Yjs        |
|                   | Auto-mount / auto-building                                              | —              | ✅               |                                                                                  |

## License

Apache 2.0.
