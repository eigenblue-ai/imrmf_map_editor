# RMF Map Editor

ImGui editor for Open-RMF building maps. The front-end talks Yjs over WebSocket
to a Rust server that holds the map in a CRDT and writes `building.yaml` back to
disk or S3. One set of sources builds two ways: a wasm app for the browser, and
a desktop app for macOS and Linux.

![The editor on an S3-backed warehouse map](docs/assets/rmf-map-editor.png)

## What it does

- Browser and desktop from one codebase, sharing the editor, the UI kit, and
  the sync protocol.
- Real-time multi-user editing over a CRDT. Concurrent edits merge, and the
  server validates before it writes.
- Local filesystem and S3 backends, with branches, snapshots, and deploys
  between branches.
- `.rmfmap` bundles: a map and every image it references in one movable file,
  laid out the way the backend lays out a building.
- Geometry tools for vertices, lanes, walls, doors, floors and measurements,
  with H / V / 45° snapping and marquee selection.

## Quick start

```bash
bazel run //:dev                          # browser, then open :30010
bazel run --config=release //app:editor   # desktop
```

Both open the same connect dialog: a local `building.yaml` or `.rmfmap`, or an
S3 bucket through a running server.
[RMF Map Generator](https://github.com/lebibi/isaacsim-rmf-map-generator) writes
maps this editor opens, straight out of an Isaac Sim stage.

## Architecture

```
model/        building.yaml parse/serialize, asset paths, .rmfmap bundles.
              No UI / fetch deps.
canvas/       Map renderer (native + wasm). Embeddable read-only.
ui/           Shared ImGui theme, widgets, fonts, icons.
view/         EditorView: modes, selection, Yjs op wiring.
app/          Front-end: wasm (:editor_wasm) and desktop (:editor).
client_rust/  Desktop CRDT client: y-sync over WebSocket + REST, as a C ABI.
server_rust/  axum/yrs WebSocket server: CRDT + REST. :core is shared with the
              desktop client.
server/       Shared buildings_api.
test/         GoogleTest suite for the C++ side. Rust tests stay inline, as
              `#[cfg(test)]` modules next to what they cover.
third_party/  Patched rmf_building_map_tools, vendored miniz.
```

The browser gets fetch, the WebSocket, and the Yjs provider from JS. The desktop
gets the same three from `client_rust`, which reuses the server's own protocol
and yaml-bridge code, so both speak the same wire format to the same server.

## Docs

- [Building and running](docs/development.md) has the flags, the platform notes
  and the tests.
- [Map bundles](docs/map-bundles.md) covers the `.rmfmap` format, image paths,
  and where maps come from.
- [Deployment](docs/deployment.md) covers the container image and remounting a
  running server.
- [Shortcuts](docs/shortcuts.md) is the key table.
- [Feature gaps](docs/feature-gaps.md) tracks what upstream traffic-editor has
  and this does not.

## License

Apache 2.0.
