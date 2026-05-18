# ImRmfMapEditor

Opinionated, ImGui-based web editor for OpenRMF building map files. The wasm
front-end talks Yjs over WebSocket to a Rust server that hosts the CRDT and
flushes `building.yaml` to disk or S3.

## Features

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

| Key | Action |
|---|---|
| `S` / `V` / `L` | Select / Vertex / Lane mode |
| `Esc` | Break the lane chain |
| `Delete` | Delete current selection |
| `Ctrl/Cmd+S` | Force flush |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `Shift` (drag) | Snap to H / V / 45° |
| `Shift` (click) | Add / toggle in selection |
| Middle-mouse / wheel | Pan / zoom |

## Layout

```
model/        building.yaml parse/serialize. No UI / fetch deps.
canvas/       Map renderer (native + wasm). Embeddable read-only.
view/         EditorView: modes, selection, Yjs op wiring.
app/          Wasm cc_binary front-end.
server_rust/  axum/yrs WebSocket server: CRDT + REST.
server/       Shared buildings_api.
third_party/  Patched rmf_building_map_tools.
```

## Run

From source:

```bash
bazel run //:dev
```

Open http://localhost:30010 and use the connect modal.

Forward server flags after `--`:

```bash
bazel run //:dev -- --port 30099 --no-validate
```

## Docker

Build the artifacts, stage them where the `Dockerfile` expects them (`dist/`,
dereferencing Bazel's output symlinks), then build and run:

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

Comparison against [`osrf/ros2multirobotbook/traffic-editor`](https://osrf.github.io/ros2multirobotbook/traffic-editor.html). `partial` = present in the data model but not surfaced in the UI.

| Area | Feature | traffic-editor | imrmf_map_editor | Notes |
|---|---|---|---|---|
| Geometry tools | Walls (`w`) | ✓ | ✗ | round-tripped in `Level::passthrough` |
| | Floor polygons | ✓ | ✗ | passthrough |
| | Hole polygons | ✓ | ✗ | passthrough |
| | Edit polygon tool | ✓ | ✗ | — |
| | Doors (hinged/sliding/etc.) | ✓ | ✗ | passthrough |
| | Lifts (multi-level, cabin, doors) | ✓ | ✗ | `Building::passthrough` |
| | Fiducials | ✓ | ✗ | no inter-level auto-transform |
| | Measurements (scale calibration) | ✓ | ✗ | mpp only via layer scale |
| | Model / asset placement | ✓ | ✗ | no thumbnail library |
| Edit ops | Move tool (`m`) | ✓ | partial | only via Vertex-mode drag |
| | Rotate tool (`r`) | ✓ | ✗ | — |
| | Toggle bidirectional (`b`) | ✓ | ✗ | edit via attribute panel only |
| | Grid snap | ✓ | partial | shift→H/V/45° only, no fixed grid |
| | Marquee select lanes | — | ✓ | |
| | Multi-vertex align H/V | — | ✓ | |
| Vertex params | Standard 11 RMF keys | ✓ | ✓ | match |
| | `mutex`, `merge_radius` | — | ✓ | extras |
| | `Vertex.z` (elevation) | ✓ | partial | in struct, not in UI |
| Lane params | bidirectional, orientation, graph_idx, speed_limit, demo_mock_*, mutex | ✓ | ✓ | match |
| | Direction arrows | ✓ | ✓ | match |
| Level metadata | `elevation` editor | ✓ | ✗ | in struct, not editable |
| | `drawing_filename` direct edit | ✓ | partial | only via Layers flow |
| | Add / rename / reorder levels | ✓ | ✗ | dropdown selector only |
| Building metadata | `name` editor | ✓ | ✗ | in struct, not editable |
| | `coordinate_system` editor | ✓ | ✗ | inherits loaded value |
| Layer | scale / yaw / translation | ✓ | ✓ | match |
| | RGB color | ✓ | ✓ | match |
| | Alpha (`color_a`) | ✓ | ✓ | match |
| | `visible` toggle | ✓ | ✓ | match |
| Sidebar | Levels panel | ✓ | ✗ | — |
| | Layers panel | ✓ | ✓ | match (overlay) |
| | Lifts panel | ✓ | ✗ | — |
| | Traffic tab + per-graph visibility (0..8) | ✓ | ✗ | all lanes always drawn |
| | Live cursor coords | ✓ | ✗ | — |
| | Graph color legend | ✓ | ✗ | — |
| Crowd sim | `human_goal_set_name` on vertex | ✓ | ✓ | match |
| | `crowd_sim` block editor (agent_groups, profiles, transitions) | ✓ | ✗ | `Building::passthrough` |
| Workflow | `building_map_generator` integration | ✓ | partial | server validates only, no UI button |
| | Nav-graph (`*.nav.yaml`) export | ✓ | ✗ | — |
| | 3D / wall-mesh preview / export | ✓ | ✗ | — |
| | Scenario / task authoring | ✓ | ✗ | — |
| Sync / storage | Local filesystem | ✓ | ✓ | match |
| | S3 backend | — | ✓ | |
| | Multi-user Yjs CRDT | — | ✓ | |
| | Browser / wasm UI | — | ✓ | |
| | Auto-mount / auto-building | — | ✓ | |

## License

Apache 2.0.
