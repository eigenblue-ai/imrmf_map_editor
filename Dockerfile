# Runtime image. Build from the repo root; bazel artifacts must be staged
# under dist/ first (see README / the build-image workflow).

FROM ubuntu:24.04 AS runtime

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
        python3 \
        tini \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/imrmf

COPY dist/server_rust/imrmf_map_editor          /opt/imrmf/imrmf_map_editor
COPY dist/server_rust/imrmf_map_editor.runfiles /opt/imrmf/imrmf_map_editor.runfiles

COPY dist/app/editor_wasm /opt/imrmf/www

COPY docker_entrypoint.sh /opt/imrmf/entrypoint.sh
RUN chmod +x /opt/imrmf/entrypoint.sh /opt/imrmf/imrmf_map_editor

ENV IMRMF_CACHE_ROOT=/var/imrmf/cache \
    IMRMF_PORT=30010 \
    IMRMF_WASM_DIR=/opt/imrmf/www \
    IMRMF_EDITOR_BIN=/opt/imrmf/imrmf_map_editor

EXPOSE 30010

ENTRYPOINT ["/usr/bin/tini", "--", "/opt/imrmf/entrypoint.sh"]
