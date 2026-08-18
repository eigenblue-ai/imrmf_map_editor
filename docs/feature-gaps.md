# Feature gaps vs upstream traffic-editor

Comparison against [`osrf/ros2multirobotbook/traffic-editor`](https://osrf.github.io/ros2multirobotbook/traffic-editor.html). Key: ✅ done · 🟡 partial · ❌ missing · — not applicable upstream.

| Area              | Feature                                                                 | traffic-editor | rmf_map_editor | Notes                                                                            |
| ----------------- | ----------------------------------------------------------------------- | -------------- | -------------- | -------------------------------------------------------------------------------- |
| Geometry tools    | Walls (`w`)                                                             | ✅             | ✅             | draw chain, select, edit textures, delete                                        |
|                   | Floor polygons                                                          | ✅             | ✅             | click a loop to define; fill + outline; param edit                               |
|                   | Hole polygons                                                           | ✅             | ✅             | added to the selected floor; outline only (fill doesn't subtract yet)            |
|                   | Edit polygon tool                                                       | ✅             | 🟡             | loop vertices move with the shared vertex drag; no add/remove-vertex-in-loop yet |
|                   | Doors (hinged/sliding/etc.)                                             | ✅             | ✅             | type + motion params, swing-arc glyph                                            |
|                   | Lifts (multi-level, cabin, doors)                                       | ✅             | ❌             | `Building::passthrough`                                                          |
|                   | Fiducials                                                               | ✅             | ❌             | no inter-level auto-transform                                                    |
|                   | Measurements (scale calibration)                                        | ✅             | ✅             | create + edit `distance`; feeds level scale                                      |
|                   | Model / asset placement                                                 | ✅             | ❌             | no thumbnail library                                                             |
| Edit ops          | Move tool (`m`)                                                         | ✅             | 🟡             | only via Vertex-mode drag                                                        |
|                   | Rotate tool (`r`)                                                       | ✅             | ❌             | —                                                                                |
|                   | Toggle bidirectional (`b`)                                              | ✅             | ❌             | edit via attribute panel only                                                    |
|                   | Grid snap                                                               | ✅             | 🟡             | shift→H/V/45° only, no fixed grid                                                |
|                   | Marquee select lanes                                                    | —              | ✅             |                                                                                  |
|                   | Multi-vertex align H/V                                                  | —              | ✅             |                                                                                  |
| Vertex params     | Standard 11 RMF keys                                                    | ✅             | ✅             | match                                                                            |
|                   | `mutex`, `merge_radius`                                                 | —              | ✅             | extras                                                                           |
|                   | `Vertex.z` (elevation)                                                  | ✅             | 🟡             | in struct, not in UI                                                             |
| Lane params       | bidirectional, orientation, graph*idx, speed_limit, demo_mock*\*, mutex | ✅             | ✅             | match                                                                            |
|                   | Direction arrows                                                        | ✅             | ✅             | match                                                                            |
| Level metadata    | `elevation` editor                                                      | ✅             | ❌             | in struct, not editable                                                          |
|                   | `drawing_filename` direct edit                                          | ✅             | 🟡             | only via Layers flow                                                             |
|                   | Add / rename / reorder levels                                           | ✅             | ❌             | dropdown selector only                                                           |
| Building metadata | `name` editor                                                           | ✅             | ❌             | in struct, not editable                                                          |
|                   | `coordinate_system` editor                                              | ✅             | ❌             | inherits loaded value                                                            |
| Layer             | scale / yaw / translation                                               | ✅             | ✅             | match                                                                            |
|                   | RGB color                                                               | ✅             | ✅             | match                                                                            |
|                   | Alpha (`color_a`)                                                       | ✅             | ✅             | match                                                                            |
|                   | `visible` toggle                                                        | ✅             | ✅             | match                                                                            |
| Sidebar           | Levels panel                                                            | ✅             | ❌             | —                                                                                |
|                   | Layers panel                                                            | ✅             | ✅             | match (overlay)                                                                  |
|                   | Lifts panel                                                             | ✅             | ❌             | —                                                                                |
|                   | Traffic tab + per-graph visibility (0..8)                               | ✅             | ❌             | all lanes always drawn                                                           |
|                   | Live cursor coords                                                      | ✅             | ❌             | —                                                                                |
|                   | Graph color legend                                                      | ✅             | ❌             | —                                                                                |
| Crowd sim         | `human_goal_set_name` on vertex                                         | ✅             | ✅             | match                                                                            |
|                   | `crowd_sim` block editor (agent_groups, profiles, transitions)          | ✅             | ❌             | `Building::passthrough`                                                          |
| Workflow          | `building_map_generator` integration                                    | ✅             | 🟡             | server validates only, no UI button                                              |
|                   | Nav-graph (`*.nav.yaml`) export                                         | ✅             | ❌             | —                                                                                |
|                   | 3D / wall-mesh preview / export                                         | ✅             | ❌             | —                                                                                |
|                   | Scenario / task authoring                                               | ✅             | ❌             | —                                                                                |
| Sync / storage    | Local filesystem                                                        | ✅             | ✅             | match                                                                            |
|                   | S3 backend                                                              | —              | ✅             |                                                                                  |
|                   | Multi-user Yjs CRDT                                                     | —              | ✅             |                                                                                  |
|                   | Browser / wasm UI                                                       | —              | ✅             |                                                                                  |
|                   | Desktop app (macOS / Linux)                                             | ✅             | ✅             | same dialog and editor as the browser; local files sync-free, S3 over Yjs        |
|                   | Auto-mount / auto-building                                              | —              | ✅             |                                                                                  |
