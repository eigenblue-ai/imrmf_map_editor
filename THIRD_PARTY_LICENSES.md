# Third-party licenses

ImRmfMapEditor is licensed under the Apache License, Version 2.0. Its text is in
`LICENSE`. This file records the third-party software that is copied into this
source tree or fetched by the build, the license each component is under, and
where that license text can be found.

Names in the License column are SPDX identifiers. Several upstream projects
offer a choice of licenses. The Taken under column records the one this project
relies on; the others remain available to anyone who prefers them.

Three artifacts are distributed:

- `//app:editor`, the desktop application.
- `//app:editor_wasm`, the browser application. Its `.data` file has the two
  fonts preloaded into it.
- `//server_rust:imrmf_map_editor` and its runfiles, which is what the container
  image built from `Dockerfile` holds.

## Copied into this source tree

| Component                     | Version          | License            | Taken under | Location                                                                    |
| ----------------------------- | ---------------- | ------------------ | ----------- | --------------------------------------------------------------------------- |
| stb_image                     | 2.30             | `MIT OR Unlicense` | MIT         | `canvas/stb_image.h`, `ui/stb_image.h`                                      |
| stb_image_write               | 1.16             | `MIT OR Unlicense` | MIT         | `ui/stb_image_write.h`                                                      |
| miniz                         | 3.1.2            | `MIT`              | MIT         | `third_party/miniz/`, text in `third_party/miniz/LICENSE.miniz`             |
| Inter                         | bundled copy     | `OFL-1.1`          | OFL-1.1     | `ui/fonts/Inter.ttf`, text in `ui/fonts/OFL.txt`                            |
| Material Design Icons webfont | bundled copy     | `Apache-2.0`       | Apache-2.0  | `ui/fonts/materialdesignicons-webfont.ttf`, terms in `ui/fonts/MDI-LICENSE` |
| IconFontCppHeaders output     | generated header | `Zlib`             | Zlib        | `ui/IconsMaterialDesignIcons.h`                                             |

Two notes on wording that has caused confusion before:

The comment at the top of `miniz.h` still carries the wording of the miniz 1.x
releases, which were placed in the public domain. From 2.x onward the project
ships an MIT license file, and that file governs the copy here. The vendored
`miniz.c`, `miniz.h`, and `LICENSE.miniz` are byte-identical to the upstream
3.1.2 release.

Both stb files offer MIT or a public domain dedication in the same header. This
project uses them under MIT and keeps the notice in place.

The Material Design Icons webfont is covered by the Pictogrammers Free License,
reproduced in `ui/fonts/MDI-LICENSE`. That document places the fonts and icons
under Apache-2.0 and the project's non-font code under MIT. Only the font is
used here, so Apache-2.0 applies.

## Fetched by the build and linked into the applications

| Component                   | Version                                    | License       | Taken under | Goes into            |
| --------------------------- | ------------------------------------------ | ------------- | ----------- | -------------------- |
| Dear ImGui (docking branch) | `92e2df59781d441d83cda284eccfe8dec8d0f7ad` | `MIT`         | MIT         | editor, editor_wasm  |
| GLFW                        | 3.3.9                                      | `Zlib`        | Zlib        | editor               |
| yaml-cpp                    | 0.8.0                                      | `MIT`         | MIT         | editor, editor_wasm  |
| nlohmann/json               | 3.12.0                                     | `MIT`         | MIT         | editor, editor_wasm  |
| curl                        | 8.12.0                                     | `curl`        | curl        | editor               |
| BoringSSL                   | 0.20251124.0                               | `Apache-2.0`  | Apache-2.0  | editor, through curl |
| Emscripten runtime support  | 4.0.17                                     | `MIT OR NCSA` | MIT         | editor_wasm          |

curl is built with BoringSSL, which is the default TLS backend of the Bazel
module. The mbedTLS and OpenSSL branches of curl's build file are not selected
and neither library is linked. curl is also built without zlib, so no separate
compression library is linked either.

The server binary is Rust and links none of the C and C++ libraries above.

Emscripten is a toolchain, but its runtime support code is compiled into
`editor_wasm_target.js` and `.wasm`, so it is listed here rather than under
build-time tools.

## In the server binary's runfiles and the container image

| Component                                                         | Version                                         | License                                                      | Taken under            |
| ----------------------------------------------------------------- | ----------------------------------------------- | ------------------------------------------------------------ | ---------------------- |
| rmf_building_map_tools (patched, see NOTICE)                      | `cd6bf2b49f0f6c8fd9decbfe07a8e97be27533f9`      | `Apache-2.0`                                                 | Apache-2.0             |
| rmf_building_map_msgs                                             | 1.6.0                                           | `Apache-2.0`                                                 | Apache-2.0             |
| rmf_site_map_msgs (from rmf_internal_msgs)                        | 3.3.1                                           | `Apache-2.0`                                                 | Apache-2.0             |
| ROS 2 Jazzy: rclpy, rcl_interfaces, ament_index, rosidl, rpyutils | rclpy 7.1.5, rosidl 4.6.6, rcl_interfaces 2.0.3 | `Apache-2.0`                                                 | Apache-2.0             |
| unique_identifier_msgs                                            | ROS 2 Jazzy                                     | `BSD-3-Clause`                                               | BSD-3-Clause           |
| Eclipse Cyclone DDS, through `librmw_cyclonedds.so`               | 0.10.5                                          | `EPL-2.0 OR BSD-3-Clause` (Eclipse Distribution License 1.0) | BSD-3-Clause (EDL 1.0) |
| CPython                                                           | 3.12                                            | `PSF-2.0`                                                    | PSF-2.0                |
| NumPy                                                             | 1.26.4                                          | `BSD-3-Clause`                                               | BSD-3-Clause           |
| PyYAML                                                            | 6.0.2 and 6.0.3                                 | `MIT`                                                        | MIT                    |
| lark-parser                                                       | 0.12.0                                          | `MIT`                                                        | MIT                    |
| libyaml                                                           | 0.2.5                                           | `MIT`                                                        | MIT                    |

Cyclone DDS is dual licensed. Taking it under the Eclipse Distribution License
1.0, which is the three-clause BSD license, avoids the source availability
obligations that EPL-2.0 attaches to modified files. Nothing here modifies it.
Its own notice file is `NOTICE.md` in the upstream tree.

The image is built `FROM ubuntu:24.04` and installs `ca-certificates`,
`libatomic1`, `python3`, and `tini` from the Ubuntu archive. Those packages
carry their own licenses and are not redistributed from this repository.

## Rust crates

Direct dependencies, at the versions the lock resolves to:

| Crate                                        | Version                 | License             | Taken under |
| -------------------------------------------- | ----------------------- | ------------------- | ----------- |
| yrs                                          | 0.21.3                  | `MIT`               | MIT         |
| axum                                         | 0.7.9                   | `MIT`               | MIT         |
| tower-http                                   | 0.5.2                   | `MIT`               | MIT         |
| tokio                                        | 1.53.1                  | `MIT`               | MIT         |
| tokio-stream                                 | 0.1.19                  | `MIT`               | MIT         |
| tokio-tungstenite                            | 0.24.0                  | `MIT`               | MIT         |
| bytes                                        | 1.12.1                  | `MIT`               | MIT         |
| tracing                                      | 0.1.44                  | `MIT`               | MIT         |
| tracing-subscriber                           | 0.3.23                  | `MIT`               | MIT         |
| futures, futures-util                        | 0.3.34                  | `MIT OR Apache-2.0` | Apache-2.0  |
| serde, serde_json                            | 1.0.229, 1.0.151        | `MIT OR Apache-2.0` | Apache-2.0  |
| serde_yaml                                   | 0.9.34                  | `MIT OR Apache-2.0` | Apache-2.0  |
| clap                                         | 4.6.6                   | `MIT OR Apache-2.0` | Apache-2.0  |
| anyhow                                       | 1.0.104                 | `MIT OR Apache-2.0` | Apache-2.0  |
| async-trait                                  | 0.1.92                  | `MIT OR Apache-2.0` | Apache-2.0  |
| tempfile                                     | 3.27.0                  | `MIT OR Apache-2.0` | Apache-2.0  |
| ureq                                         | 2.12.1                  | `MIT OR Apache-2.0` | Apache-2.0  |
| aws-config, aws-credential-types, aws-sdk-s3 | 1.8.17, 1.2.14, 1.134.0 | `Apache-2.0`        | Apache-2.0  |

yrs was listed as `Apache-2.0 OR MIT` in earlier revisions of this file. Its
`Cargo.toml` declares `MIT` alone, and that is what applies.

The closure cargo-bazel resolves from the committed lock, covering the server,
the desktop client, and the crates their builds need, is 336 crate versions.
The license expressions that occur in it are:

- `MIT`, `Apache-2.0`, and the various spellings of the choice between them,
  which together account for 272 of the 336.
- `Unicode-3.0` for the ICU4X crates (`icu_*`, `zerovec`, `yoke`, `tinystr`,
  `writeable`, `litemap`, `potential_utf`, `zerotrie`, `zerofrom`).
- `ISC` for `rustls-webpki` and `untrusted`, `Apache-2.0 AND ISC` for `ring`,
  and `ISC AND (Apache-2.0 OR ISC)` for `aws-lc-rs` and `aws-lc-sys`.
- `CDLA-Permissive-2.0` for `webpki-roots`, which carries Mozilla's CA
  certificate set as data.
- `BSD-3-Clause` for `subtle`, `MIT AND BSD-3-Clause` for `matchit`, and
  `BSD-2-Clause OR Apache-2.0 OR MIT` for `zerocopy`.
- `Zlib` for `foldhash`, `Apache-2.0 OR BSL-1.0` for `ryu`,
  `0BSD OR MIT OR Apache-2.0` for `adler2`, `CC0-1.0 OR MIT-0 OR Apache-2.0`
  for `dunce`, and `Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT` for
  the `wasi` crates.

Every crate in that closure declares a license, and none of them is copyleft.
There is no GPL, LGPL, AGPL, MPL, EPL, or CDDL crate in the tree.

Every crate declares its license in `Cargo.toml`, and each crate's own license
files travel with its source. To rebuild the inventory:

```bash
cd "$(bazel info output_base)/external"
for d in rules_rust*crate+crates__*/; do
  printf '%s\t%s\n' "${d##*crates__}" \
    "$(grep -m1 '^license' "$d/Cargo.toml" | cut -d'"' -f2)"
done | sort
```

## Build-time only, not distributed

These run during the build and no part of them is linked into or shipped with
the artifacts above.

| Component                                                                                                 | License       | Note                                                                                                                                                                                                   |
| --------------------------------------------------------------------------------------------------------- | ------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| rules_cc, rules_python, rules_rust, rules_shell, apple_support, rules_foreign_cc, bazel_skylib, platforms | `Apache-2.0`  | Bazel rule sets                                                                                                                                                                                        |
| rules_ros2 (`56ad1dfa`, patched by `extensions/rules_ros2_srv_support.patch`)                             | `Apache-2.0`  | ROS 2 build rules                                                                                                                                                                                      |
| emsdk                                                                                                     | `MIT OR NCSA` | toolchain; its runtime support code is listed above                                                                                                                                                    |
| empy                                                                                                      | 3.3.4, LGPL   | template engine invoked by the rosidl generators. It emits code from ROS 2's own Apache-2.0 templates. No empy code is copied into the generated sources and empy is not present in any runfiles tree. |

## Reproducing the notices

Apache-2.0 requires that recipients get a copy of the license and that
attribution notices travel with the work. The Apache-2.0 text is `LICENSE`, and
the attributions are in `NOTICE`. The container image built from `Dockerfile`
carries `LICENSE`, `NOTICE`, this file, and the two font licenses under
`/opt/imrmf/licenses`. The desktop application carries `ui/fonts/OFL.txt` and
`ui/fonts/MDI-LICENSE` in its runfiles next to the fonts they cover.

Copyright notices that the permissive licenses above require be preserved:

```
GLFW
  Copyright (c) 2002-2006 Marcus Geelnard
  Copyright (c) 2006-2019 Camilla Löwy

curl
  Copyright (c) 1996 - 2025, Daniel Stenberg, <daniel@haxx.se>, and many
  contributors

yaml-cpp
  Copyright (c) 2008-2015 Jesse Beder

libyaml
  Copyright (c) 2017-2020 Ingy döt Net
  Copyright (c) 2006-2016 Kirill Simonov

PyYAML
  Copyright (c) 2017-2021 Ingy döt Net
  Copyright (c) 2006-2016 Kirill Simonov

Dear ImGui
  Copyright (c) 2014-2025 Omar Cornut

nlohmann/json
  Copyright (c) 2013-2025 Niels Lohmann

miniz
  Copyright 2013-2014 RAD Game Tools and Valve Software
  Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

stb_image, stb_image_write
  Copyright (c) 2017 Sean Barrett

Inter
  Copyright 2020 The Inter Project Authors

lark-parser
  Copyright (c) 2017 Erez Shinan

NumPy
  Copyright (c) 2005-2023, NumPy Developers

unique_identifier_msgs
  Copyright (C) 2012, Jack O'Quin
```
