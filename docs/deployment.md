# Deployment

## Docker

Build the artifacts, stage them where the `Dockerfile` expects them (`dist/`,
dereferencing Bazel's output symlinks), then build and run:

The image includes `building_map_server`, the ROS2 `/map` publisher the
entrypoint starts. It builds by default. macOS turns it off via `.bazelrc`
(`--define=ros2=false`) because rules_ros2 does not build there.

```bash
bazel build //server_rust:rmf_map_editor
bazel build --config=wasm //app:editor_wasm

mkdir -p dist/server_rust dist/app
cp -RL bazel-bin/server_rust/rmf_map_editor          dist/server_rust/
cp -RL bazel-bin/server_rust/rmf_map_editor.runfiles dist/server_rust/
cp -RL bazel-bin/app/editor_wasm                       dist/app/

docker build -t imrmf-map-editor:dev -f Dockerfile .
docker run --rm -p 30010:30010 -v "$(pwd)/maps:/var/imrmf/cache" imrmf-map-editor:dev
```

CI builds and publishes this image to `ghcr.io/eigenblue-ai/rmf_map_editor`
(see `.github/workflows/build-image.yml`).

## Remounting a running server

`IMRMF_AUTO_MOUNT_KIND` also locks the mount: `POST /mount` and `/unmount`
return 409. `IMRMF_ALLOW_REMOUNT=1` keeps them open so a client can point the
server somewhere else.

The server has no authentication and its CORS policy allows any origin, so turn
this on only on a trusted network. With it on, anything that can reach the
server can change which backend everyone is editing, and remounting resets the
shared document for every connected client.

Credentials are never sent to a client. `GET /config` reports only whether the
server holds them, and a mount request with blank credentials reuses the ones
already in place, but only when it targets the same endpoint and region.
Changing either means supplying your own, so a caller cannot aim the server's
keys at a host of their choosing.
