// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::collections::hash_map::DefaultHasher;
use std::collections::HashMap;
use std::hash::{Hash, Hasher};
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, Context, Result};
use tokio::sync::{broadcast, Mutex, RwLock};
use yrs::{Doc, ReadTxn, Transact};

use crate::storage::{MountConfig, MountInfo, SnapshotInfo, Storage};
use crate::sync;
use crate::validate::Validator;
use crate::yaml_bridge;

pub struct AppState {
    pub doc: Doc,
    pub broadcast: broadcast::Sender<Vec<u8>>,
    pub validator: Option<Validator>,

    /// Active storage backend, set by POST /mount and cleared by /unmount
    storage: RwLock<Option<Arc<dyn Storage>>>,
    /// Building id loaded into the Doc, if any.
    mounted_building: RwLock<Option<String>>,

    locked: bool,

    /// Cache of generated nav graph YAML keyed by (yaml hash, fleet id)
    nav_graph_cache: Mutex<HashMap<(u64, String), String>>,

    inner: Mutex<Inner>,
}

struct Inner {
    dirty: bool,
    /// Hash of the YAML we last wrote to the active storage.
    /// Used to detect external edits via the local cache file's stat.
    last_disk_hash: u64,
    /// The last YAML known to pass validation. /revert_to_last_valid pulls
    /// from here.
    last_valid_yaml: Option<String>,
    validation: ValidationStatus,
    last_validated_hash: u64,
}

#[derive(Clone, Debug, serde::Serialize)]
#[serde(tag = "status", rename_all = "snake_case")]
pub enum ValidationStatus {
    Unknown,
    Ok { at_unix_secs: u64 },
    Failed { at_unix_secs: u64, message: String },
}

#[derive(Clone, Debug, serde::Serialize)]
#[serde(tag = "state", rename_all = "snake_case")]
pub enum MountStatus {
    Unmounted,
    Mounted {
        building_id: Option<String>,
        info: MountInfo,
    },
}

impl AppState {
    pub fn new(validator: Option<Validator>, locked: bool) -> Arc<Self> {
        let (broadcast, _) = broadcast::channel(1024);
        Arc::new(Self {
            doc: Doc::new(),
            broadcast,
            validator,
            storage: RwLock::new(None),
            mounted_building: RwLock::new(None),
            locked,
            nav_graph_cache: Mutex::new(HashMap::new()),
            inner: Mutex::new(Inner {
                dirty: false,
                last_disk_hash: 0,
                last_valid_yaml: None,
                validation: ValidationStatus::Unknown,
                last_validated_hash: 0,
            }),
        })
    }

    pub fn is_locked(&self) -> bool {
        self.locked
    }

    /// Build (or fetch cached) nav graph YAML for `(building_id, fleet_id)`.
    pub async fn nav_graph(&self, building_id: &str, fleet_id: &str) -> Result<String> {
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        let yaml = storage.read_yaml(building_id).await?;
        let key = (hash_bytes(yaml.as_bytes()), fleet_id.to_string());
        {
            let cache = self.nav_graph_cache.lock().await;
            if let Some(v) = cache.get(&key) {
                return Ok(v.clone());
            }
        }
        let validator = self
            .validator
            .clone()
            .ok_or_else(|| anyhow!("validator not configured"))?;
        let out = validator.generate_nav_graph(&yaml, fleet_id).await?;
        self.nav_graph_cache.lock().await.insert(key, out.clone());
        Ok(out)
    }

    /// Install a new storage backend and load `building_id` into the Doc
    pub async fn mount(
        &self,
        cfg: MountConfig,
        cache_root: &std::path::Path,
    ) -> Result<Vec<String>> {
        let storage: Arc<dyn Storage> = match cfg {
            MountConfig::Local { path } => {
                Arc::new(crate::storage::local::LocalFsStorage::new(path.into())?)
            }
            MountConfig::S3 {
                bucket,
                prefix,
                region,
                access_key_id,
                secret_access_key,
                session_token,
                endpoint_url,
            } => Arc::new(
                crate::storage::s3::S3Storage::new(crate::storage::s3::S3Config {
                    bucket,
                    prefix,
                    region,
                    access_key_id,
                    secret_access_key,
                    session_token,
                    endpoint_url,
                    cache_root: cache_root.to_path_buf(),
                })
                .await?,
            ),
        };
        let buildings = storage.list_buildings().await?;
        {
            let mut slot = self.storage.write().await;
            *slot = Some(storage);
        }
        *self.mounted_building.write().await = None;
        // Reset doc + validation so a stale prior session can't leak through.
        self.reseed_doc("").await.ok();
        let mut inner = self.inner.lock().await;
        inner.validation = ValidationStatus::Unknown;
        inner.last_valid_yaml = None;
        inner.last_validated_hash = 0;
        Ok(buildings)
    }

    pub async fn load_building(&self, building_id: &str) -> Result<()> {
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        if self
            .mounted_building
            .read()
            .await
            .as_ref()
            .map(|b| b == building_id)
            .unwrap_or(false)
        {
            return Ok(());
        }
        let yaml = storage.read_yaml(building_id).await?;
        self.reseed_doc(&yaml).await?;
        *self.mounted_building.write().await = Some(building_id.to_string());
        let mut inner = self.inner.lock().await;
        inner.last_disk_hash = hash_bytes(yaml.as_bytes());
        inner.last_valid_yaml = Some(yaml.clone());
        inner.dirty = false;
        drop(inner);

        // Mirror every layer asset referenced by the yaml into the local cache
        for path in collect_asset_paths(&yaml) {
            if let Err(e) = storage.mirror_asset(building_id, &path).await {
                tracing::warn!("mirror_asset {building_id}/{path} failed: {e:#}");
            }
        }
        Ok(())
    }

    /// Replace the building's yaml with `content`
    pub async fn replace_building(&self, building_id: &str, content: &str) -> Result<()> {
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        storage.write_yaml(building_id, content).await?;
        let is_current = self
            .mounted_building
            .read()
            .await
            .as_ref()
            .map(|b| b == building_id)
            .unwrap_or(false);
        if is_current {
            let sv_before = self.doc.transact().state_vector();
            self.reseed_doc(content).await?;
            let update_bytes = self.doc.transact().encode_state_as_update_v1(&sv_before);
            if !update_bytes.is_empty() {
                let _ = self
                    .broadcast
                    .send(sync::encode_update_message(&update_bytes));
            }
            let mut inner = self.inner.lock().await;
            inner.last_disk_hash = hash_bytes(content.as_bytes());
            inner.last_valid_yaml = Some(content.to_string());
            inner.dirty = false;
        }
        Ok(())
    }

    pub async fn unmount(&self) -> Result<()> {
        *self.storage.write().await = None;
        *self.mounted_building.write().await = None;
        self.reseed_doc("").await.ok();
        let mut inner = self.inner.lock().await;
        inner.dirty = false;
        inner.last_disk_hash = 0;
        inner.last_valid_yaml = None;
        inner.validation = ValidationStatus::Unknown;
        inner.last_validated_hash = 0;
        Ok(())
    }

    pub async fn mount_status(&self) -> MountStatus {
        match self.storage.read().await.as_ref() {
            None => MountStatus::Unmounted,
            Some(s) => MountStatus::Mounted {
                building_id: self.mounted_building.read().await.clone(),
                info: s.describe(),
            },
        }
    }

    pub async fn storage(&self) -> Option<Arc<dyn Storage>> {
        self.storage.read().await.as_ref().cloned()
    }

    pub async fn mounted_building(&self) -> Option<String> {
        self.mounted_building.read().await.clone()
    }

    pub async fn mark_dirty(&self) {
        self.inner.lock().await.dirty = true;
    }

    pub async fn flush_if_dirty(&self) -> Result<()> {
        let Some(storage) = self.storage().await else {
            return Ok(());
        };
        let Some(building_id) = self.mounted_building().await else {
            return Ok(());
        };

        let mut inner = self.inner.lock().await;

        let cache_path = storage.local_cache_path(&building_id);
        if cache_path.exists() {
            if let Ok(current) = tokio::fs::read_to_string(&cache_path).await {
                let current_hash = hash_bytes(current.as_bytes());
                if current_hash != inner.last_disk_hash {
                    tracing::info!(
                        "external edit detected in {}, reseeding Doc",
                        cache_path.display()
                    );
                    let sv_before = self.doc.transact().state_vector();
                    yaml_bridge::seed_doc(&self.doc, &current)?;
                    let update_bytes = self.doc.transact().encode_state_as_update_v1(&sv_before);
                    if !update_bytes.is_empty() {
                        let _ = self
                            .broadcast
                            .send(sync::encode_update_message(&update_bytes));
                    }
                    inner.last_disk_hash = current_hash;
                    inner.dirty = false;
                    drop(inner);
                    if self.validator.is_some() {
                        let _ = self.run_validation_if_changed(&current).await;
                    }
                    return Ok(());
                }
            }
        }

        if !inner.dirty {
            let needs_initial =
                self.validator.is_some() && matches!(inner.validation, ValidationStatus::Unknown);
            drop(inner);
            if needs_initial {
                let yaml = {
                    let txn = self.doc.transact();
                    yaml_bridge::serialize_doc(&txn)?
                };
                if !yaml.trim().is_empty() {
                    let _ = self.run_validation_if_changed(&yaml).await;
                }
            }
            return Ok(());
        }

        let yaml = {
            let txn = self.doc.transact();
            yaml_bridge::serialize_doc(&txn)?
        };
        storage
            .write_yaml(&building_id, &yaml)
            .await
            .with_context(|| format!("write yaml for {building_id}"))?;
        inner.last_disk_hash = hash_bytes(yaml.as_bytes());
        inner.dirty = false;
        drop(inner);
        if self.validator.is_some() {
            let _ = self.run_validation_if_changed(&yaml).await;
        }
        Ok(())
    }

    async fn run_validation_if_changed(&self, yaml: &str) -> Result<()> {
        let Some(validator) = self.validator.clone() else {
            return Ok(());
        };
        let hash = hash_bytes(yaml.as_bytes());
        {
            let inner = self.inner.lock().await;
            if inner.last_validated_hash == hash {
                return Ok(());
            }
        }
        let outcome = validator.validate(yaml).await;
        let now = unix_secs_now();
        let mut inner = self.inner.lock().await;
        inner.last_validated_hash = hash;
        match outcome {
            Ok(()) => {
                inner.last_valid_yaml = Some(yaml.to_string());
                inner.validation = ValidationStatus::Ok { at_unix_secs: now };
            }
            Err(e) => {
                let message = format!("{e:#}");
                tracing::warn!("validation failed: {message}");
                inner.validation = ValidationStatus::Failed {
                    at_unix_secs: now,
                    message,
                };
            }
        }
        Ok(())
    }

    pub async fn validation_status(&self) -> ValidationStatus {
        self.inner.lock().await.validation.clone()
    }

    pub async fn revert_to_last_valid(&self) -> Result<()> {
        let yaml = {
            let inner = self.inner.lock().await;
            inner.last_valid_yaml.clone()
        };
        let yaml = yaml.ok_or_else(|| anyhow!("no validated snapshot available"))?;
        let sv_before = self.doc.transact().state_vector();
        yaml_bridge::seed_doc(&self.doc, &yaml)?;
        let update_bytes = self.doc.transact().encode_state_as_update_v1(&sv_before);
        if !update_bytes.is_empty() {
            let _ = self
                .broadcast
                .send(sync::encode_update_message(&update_bytes));
        }
        let mut inner = self.inner.lock().await;
        inner.dirty = true;
        inner.validation = ValidationStatus::Ok {
            at_unix_secs: unix_secs_now(),
        };
        Ok(())
    }

    /// Force flush, used on shutdown. Best-effort against the mounted backend.
    pub async fn flush_force(&self) -> Result<()> {
        let Some(storage) = self.storage().await else {
            return Ok(());
        };
        let Some(building_id) = self.mounted_building().await else {
            return Ok(());
        };
        let yaml = {
            let txn = self.doc.transact();
            yaml_bridge::serialize_doc(&txn)?
        };
        storage.write_yaml(&building_id, &yaml).await?;
        let mut inner = self.inner.lock().await;
        inner.last_disk_hash = hash_bytes(yaml.as_bytes());
        inner.dirty = false;
        Ok(())
    }

    async fn reseed_doc(&self, yaml: &str) -> Result<()> {
        yaml_bridge::seed_doc(&self.doc, yaml).map(|_| ())
    }

    pub async fn list_snapshots(&self, building_id: &str) -> Result<Vec<SnapshotInfo>> {
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        storage.list_snapshots(building_id).await
    }

    pub async fn restore_snapshot(&self, building_id: &str, dir: &str) -> Result<()> {
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        let yaml = storage.read_snapshot_yaml(building_id, dir).await?;
        for path in collect_asset_paths(&yaml) {
            match storage.read_snapshot_asset(building_id, dir, &path).await {
                Ok(bytes) => {
                    if let Err(e) = storage.write_asset(building_id, &path, bytes).await {
                        tracing::warn!("restore write_asset {path}: {e:#}");
                    }
                }
                Err(e) => tracing::warn!("restore skip asset {path}: {e:#}"),
            }
        }
        storage.write_yaml(building_id, &yaml).await?;
        let is_current = self
            .mounted_building
            .read()
            .await
            .as_ref()
            .map(|b| b == building_id)
            .unwrap_or(false);
        if is_current {
            let sv_before = self.doc.transact().state_vector();
            self.reseed_doc(&yaml).await?;
            let update_bytes = self.doc.transact().encode_state_as_update_v1(&sv_before);
            if !update_bytes.is_empty() {
                let _ = self
                    .broadcast
                    .send(sync::encode_update_message(&update_bytes));
            }
            let mut inner = self.inner.lock().await;
            inner.last_disk_hash = hash_bytes(yaml.as_bytes());
            inner.last_valid_yaml = Some(yaml.clone());
            inner.dirty = false;
        }
        Ok(())
    }

    pub async fn create_snapshot(&self, building_id: &str) -> Result<SnapshotInfo> {
        self.flush_if_dirty().await.ok();
        let storage = self
            .storage
            .read()
            .await
            .as_ref()
            .cloned()
            .ok_or_else(|| anyhow!("no storage mounted"))?;
        let yaml = {
            let txn = self.doc.transact();
            yaml_bridge::serialize_doc(&txn)?
        };
        let sha = short_sha(yaml.as_bytes());
        let created_at = unix_secs_now() as i64;
        let snap = SnapshotInfo {
            dir: format!("{created_at}-{sha}"),
            sha,
            created_at,
        };
        let mut assets = Vec::new();
        for path in collect_asset_paths(&yaml) {
            match storage.read_asset(building_id, &path).await {
                Ok(bytes) => assets.push((path, bytes)),
                Err(e) => tracing::warn!("snapshot skip asset {path}: {e:#}"),
            }
        }
        storage
            .create_snapshot(building_id, &snap, &yaml, &assets)
            .await?;
        Ok(snap)
    }
}

fn short_sha(bytes: &[u8]) -> String {
    let mut h = DefaultHasher::new();
    bytes.hash(&mut h);
    format!("{:07x}", h.finish() & 0x0fff_ffff)
}

fn unix_secs_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

fn hash_bytes(bytes: &[u8]) -> u64 {
    let mut h = DefaultHasher::new();
    bytes.hash(&mut h);
    h.finish()
}

fn collect_asset_paths(yaml_text: &str) -> Vec<String> {
    let Ok(value) = serde_yaml::from_str::<serde_yaml::Value>(yaml_text) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    walk_for_filenames(&value, &mut out);
    out.sort();
    out.dedup();
    out
}

fn walk_for_filenames(value: &serde_yaml::Value, out: &mut Vec<String>) {
    match value {
        serde_yaml::Value::Mapping(map) => {
            for (k, v) in map {
                if let (Some(key), Some(s)) = (k.as_str(), v.as_str()) {
                    if key == "filename" && !s.is_empty() && !s.ends_with(".yaml") {
                        out.push(s.to_string());
                    }
                }
                walk_for_filenames(v, out);
            }
        }
        serde_yaml::Value::Sequence(seq) => {
            for v in seq {
                walk_for_filenames(v, out);
            }
        }
        _ => {}
    }
}
