# Third-party licenses

This product bundles or links against the components below. Full license
texts are available at the linked URLs.

## Bundled in source tree

| Component | Version / commit | License | Location |
|---|---|---|---|
| stb_image | v2.30 | MIT or Public Domain | `canvas/stb_image.h` |

## Fetched at build time (Bazel `http_archive` / Rust crates)

### C/C++

| Component | Version / commit | License |
|---|---|---|
| open-rmf/rmf_traffic_editor (rmf_building_map_tools) | `cd6bf2b49f0f6c8fd9decbfe07a8e97be27533f9` | Apache-2.0 |
| open-rmf/rmf_building_map_msgs | `1.6.0` | Apache-2.0 |
| open-rmf/rmf_internal_msgs (rmf_site_map_msgs) | `3.3.1` | Apache-2.0 |
| ocornut/dear imgui (docking) | `92e2df59781d441d83cda284eccfe8dec8d0f7ad` | MIT |
| emscripten / emsdk | `4.0.17` | MIT and NCSA |
| yaml-cpp | bazel module | MIT |

### Rust crates

| Crate | License |
|---|---|
| yrs | Apache-2.0 OR MIT |
| axum, tower-http | MIT |
| tokio, tokio-stream, futures, futures-util | MIT |
| serde, serde_yaml, serde_json | MIT OR Apache-2.0 |
| clap | MIT OR Apache-2.0 |
| anyhow | MIT OR Apache-2.0 |
| tracing, tracing-subscriber | MIT |
| async-trait | MIT OR Apache-2.0 |
| bytes | MIT |
| aws-sdk-s3, aws-config, aws-credential-types | Apache-2.0 |

## Apache-2.0 components

Their license text is reproduced in this product as `LICENSE`. Their
NOTICE attributions (where present) are reproduced or summarised in the
`NOTICE` file.

## MIT / MIT-OR-Apache-2.0 components

Their permission notices are preserved in the source of each component as
fetched from upstream. No source-level changes are made to MIT-licensed
dependencies in this repository.
