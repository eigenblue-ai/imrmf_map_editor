# Building and running

## Browser

```bash
bazel run //:dev
```

Open http://localhost:30010 and use the connect modal.

Forward server flags after `--`:

```bash
bazel run //:dev -- --port 30099 --no-validate
```

## Desktop

```bash
bazel run --config=release //app:editor
```

`--config=release` builds optimized. The default unoptimized build is fine for
iterating on the code, but each edit re-serializes the map and reconciles it
into the CRDT document, and that work is several times slower unoptimized --
enough to be felt as lag on a large map.

Opens a dialog first, then the editor window once a building is chosen. The
dialog is the same one the browser shows, with the same two backends:

- **Local file** reads and writes the map directly, with no server and no CRDT.
  `Ctrl/Cmd+S` writes it back. The Format control picks between a plain
  `building.yaml` and a `.rmfmap` bundle. macOS uses the system open/save
  panels, elsewhere the dialog browses the filesystem itself.
- **S3 Bucket** mounts through a running server and then syncs over Yjs, so
  desktop and browser clients edit the same building together.

Snapshots and branches are browser-only for now.

On Linux the build needs the X11 and GL development headers, which GLFW's X11
backend and the `-lX11` / `-lGL` link flags pick up from the system:

```bash
sudo apt-get install -y libgl1-mesa-dev mesa-common-dev xorg-dev
```

Check the client against a running server without the UI:

```bash
bazel run //client_rust:smoke_test -- http://localhost:30010 <building-id>
```

## Platform notes

macOS needs the Apple CC toolchain for glfw's objc backend, which `.bazelrc`
selects under `build:macos`. The desktop app hides the system title bar there
and draws its own, keeping the window's shadow and its close/minimise/zoom
buttons. Linux keeps the system title bar and uses the in-app file browser.

## Tests

```bash
bazel test //test/...                      # C++: model, bundles, canvas, cost
bazel test //server_rust/... //client_rust/...   # Rust
```

`//test:edit_cost_test` is a regression guard rather than a unit test: it
asserts that recording one edit stays orders of magnitude cheaper than a yaml
round trip of the whole map, which is what a file session used to pay per click.
It prints the numbers it measured, so a run doubles as a benchmark.

`//test:yaml_io_test` also takes `--roundtrip <file.building.yaml>...`, which
round-trips real maps and reports geometry counts instead of running the cases.

CI (`.github/workflows/ci.yml`) runs the same commands on every pull request,
plus the wasm and macOS desktop builds, and the GHCR image only publishes once
the tests pass.
