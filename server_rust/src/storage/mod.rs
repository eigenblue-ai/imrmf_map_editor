// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

// Storage backends behind a single trait. A backend is mounted at runtime via
// POST /mount and lives in AppState as `Option<Arc<dyn Storage>>`. The Rust
// server is always the single writer, regardless of which backend is in use,
// so the ROS2 building_map_server side-process can watch the same local cache
// file for both local and S3 backends.

use std::path::PathBuf;

use anyhow::Result;
use async_trait::async_trait;
use bytes::Bytes;
use serde::{Deserialize, Serialize};

pub mod local;
pub mod s3;

#[async_trait]
pub trait Storage: Send + Sync {
    /// Building ids the backend can serve. For LocalFs this is the subdirs of
    /// the root that contain a `<id>/<id>.building.yaml`. For S3 it's the
    /// keys under the prefix that match the same shape.
    async fn list_buildings(&self) -> Result<Vec<String>>;

    /// Read the building.yaml for a given building. Pulls from the backend on
    /// every call, but also writes through the local cache so the ROS2 node
    /// sees a real file on disk it can `open()` and inotify.
    async fn read_yaml(&self, building_id: &str) -> Result<String>;

    /// Persist new yaml content. Writes to the backend AND the local cache
    /// atomically (cache first, then backend, then promote cache tmp to real
    /// path).
    async fn write_yaml(&self, building_id: &str, content: &str) -> Result<()>;

    /// Read a layer asset (PNG floorplan, top-view image, etc.) by its
    /// `building/<path>` slash-joined location. Used by /layer_asset.
    async fn read_asset(&self, building_id: &str, path: &str) -> Result<Bytes>;

    /// Write a layer asset. Used by PUT /layer_asset (e.g. TopView captures).
    async fn write_asset(&self, building_id: &str, path: &str, bytes: Bytes) -> Result<()>;

    /// Make an asset available on the local filesystem alongside the cached
    /// yaml so the ROS2 building_map_server can `open()` it. For LocalFs the
    /// file already IS on disk; default impl is no-op. S3 overrides to mirror.
    async fn mirror_asset(&self, _building_id: &str, _path: &str) -> Result<()> {
        Ok(())
    }

    /// The local disk path the ROS2 node should watch. Both backends keep a
    /// readable copy here, even when the source of truth is remote.
    fn local_cache_path(&self, building_id: &str) -> PathBuf;

    /// Human-readable summary, for GET /mount/status. Must not include
    /// credentials.
    fn describe(&self) -> MountInfo;
}

/// Serialized form of an active mount, returned by GET /mount/status. Fields
/// are deliberately credential-free.
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MountInfo {
    Local {
        path: String,
    },
    S3 {
        bucket: String,
        prefix: String,
        region: String,
    },
}

/// Wire format for POST /mount. The discriminator matches the `kind` field
/// in MountInfo so a client can round-trip status back into a mount request
/// (minus the secret, which they keep client-side).
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum MountConfig {
    Local {
        path: String,
    },
    S3 {
        bucket: String,
        #[serde(default)]
        prefix: String,
        region: String,
        access_key_id: String,
        secret_access_key: String,
        #[serde(default)]
        session_token: Option<String>,
        #[serde(default)]
        endpoint_url: Option<String>,
    },
}
