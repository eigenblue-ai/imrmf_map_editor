// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::RwLock;

use anyhow::{anyhow, Context, Result};
use async_trait::async_trait;
use bytes::Bytes;

use super::{MountInfo, SnapshotInfo, Storage};

/// Filesystem-backed storage. Accepts two layouts:
///
///     A. Multi-building root containing `<id>/<id>.building.yaml` subdirs. `list_buildings`
///     returns all such ids.
///     B. Single-building root containing `<basename>.building.yaml` directly, where `basename`
///     becomes the building id.
///
/// Both layouts get discovered at mount time and cached as id
pub struct LocalFsStorage {
    root: PathBuf,
    yaml_paths: RwLock<HashMap<String, PathBuf>>,
}

impl LocalFsStorage {
    pub fn new(root: PathBuf) -> Result<Self> {
        let canonical = root
            .canonicalize()
            .with_context(|| format!("root not accessible: {}", root.display()))?;
        if !canonical.is_dir() {
            return Err(anyhow!("root is not a directory: {}", canonical.display()));
        }
        Ok(Self {
            root: canonical,
            yaml_paths: RwLock::new(HashMap::new()),
        })
    }

    /// Walk the root and collect every building yaml we know how to address.
    fn rescan(&self) -> std::io::Result<HashMap<String, PathBuf>> {
        let mut out = HashMap::new();

        // Layout B: root itself contains *.building.yaml.
        for entry in std::fs::read_dir(&self.root)? {
            let entry = entry?;
            let name = entry.file_name();
            let Some(name_str) = name.to_str() else {
                continue;
            };
            if name_str.ends_with(".building.yaml") {
                let id = name_str.trim_end_matches(".building.yaml").to_string();
                if !id.is_empty() {
                    out.insert(id, entry.path());
                }
            }
        }

        // Layout A: <subdir>/<subdir>.building.yaml.
        for entry in std::fs::read_dir(&self.root)? {
            let entry = entry?;
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            let Some(dir_name) = path.file_name().and_then(|s| s.to_str()) else {
                continue;
            };
            let same = path.join(format!("{dir_name}.building.yaml"));
            if same.is_file() {
                out.entry(dir_name.to_string()).or_insert(same);
                continue;
            }
            // Also accept the first *.building.yaml inside the subdir even if its basename doesn't
            // match the dir name.
            if let Ok(rd) = std::fs::read_dir(&path) {
                for inner in rd.flatten() {
                    let n = inner.file_name();
                    let Some(ns) = n.to_str() else { continue };
                    if ns.ends_with(".building.yaml") {
                        let id = ns.trim_end_matches(".building.yaml").to_string();
                        if !id.is_empty() {
                            out.entry(id).or_insert(inner.path());
                        }
                        break;
                    }
                }
            }
        }

        Ok(out)
    }

    fn yaml_path_for(&self, building_id: &str) -> Option<PathBuf> {
        self.yaml_paths.read().ok()?.get(building_id).cloned()
    }

    fn write_path_for(&self, building_id: &str) -> PathBuf {
        // For known ids, write back where we read. For brand-new ids, default to the standard
        // `<root>/<id>/<id>.building.yaml` layout.
        self.yaml_path_for(building_id).unwrap_or_else(|| {
            self.root
                .join(building_id)
                .join(format!("{building_id}.building.yaml"))
        })
    }

    fn asset_root_for(&self, building_id: &str) -> PathBuf {
        match self.yaml_path_for(building_id) {
            Some(p) => p
                .parent()
                .map(PathBuf::from)
                .unwrap_or_else(|| self.root.clone()),
            None => self.root.join(building_id),
        }
    }

    /// We require the resolved path to stay under `self.root`.
    fn check_inside_root(&self, p: &Path) -> Result<PathBuf> {
        let resolved = p
            .canonicalize()
            .with_context(|| format!("cannot resolve {}", p.display()))?;
        if !resolved.starts_with(&self.root) {
            return Err(anyhow!("path escapes storage root: {}", resolved.display()));
        }
        Ok(resolved)
    }
}

#[async_trait]
impl Storage for LocalFsStorage {
    async fn list_buildings(&self) -> Result<Vec<String>> {
        let scanned = self.rescan()?;
        let mut ids: Vec<String> = scanned.keys().cloned().collect();
        ids.sort();
        *self.yaml_paths.write().unwrap() = scanned;
        Ok(ids)
    }

    async fn read_yaml(&self, building_id: &str) -> Result<String> {
        if self.yaml_path_for(building_id).is_none() {
            let _ = self.rescan().map(|m| {
                *self.yaml_paths.write().unwrap() = m;
            });
        }
        let p = self
            .yaml_path_for(building_id)
            .ok_or_else(|| anyhow!("no building '{building_id}' under {}", self.root.display()))?;
        tokio::fs::read_to_string(&p)
            .await
            .with_context(|| format!("read {}", p.display()))
    }

    async fn write_yaml(&self, building_id: &str, content: &str) -> Result<()> {
        let p = self.write_path_for(building_id);
        if let Some(parent) = p.parent() {
            tokio::fs::create_dir_all(parent).await.ok();
        }
        let mut tmp = p.clone();
        tmp.as_mut_os_string().push(".tmp");
        tokio::fs::write(&tmp, content).await?;
        tokio::fs::rename(&tmp, &p).await?;
        self.yaml_paths
            .write()
            .unwrap()
            .insert(building_id.to_string(), p);
        Ok(())
    }

    async fn read_asset(&self, building_id: &str, path: &str) -> Result<Bytes> {
        let candidate = self.asset_root_for(building_id).join(path);
        let resolved = self.check_inside_root(&candidate)?;
        let bytes = tokio::fs::read(&resolved).await?;
        Ok(Bytes::from(bytes))
    }

    async fn write_asset(&self, building_id: &str, path: &str, bytes: Bytes) -> Result<()> {
        let dest = self.asset_root_for(building_id).join(path);
        if let Some(parent) = dest.parent() {
            tokio::fs::create_dir_all(parent)
                .await
                .with_context(|| format!("create dir {}", parent.display()))?;
        }
        // canonicalize() requires the file exist, check the parent instead.
        if let Some(parent) = dest.parent() {
            let resolved_parent = parent
                .canonicalize()
                .with_context(|| format!("cannot resolve {}", parent.display()))?;
            if !resolved_parent.starts_with(&self.root) {
                return Err(anyhow!(
                    "asset path escapes storage root: {}",
                    dest.display()
                ));
            }
        }
        let mut tmp = dest.clone();
        tmp.as_mut_os_string().push(".tmp");
        tokio::fs::write(&tmp, &bytes).await?;
        tokio::fs::rename(&tmp, &dest).await?;
        Ok(())
    }

    fn local_cache_path(&self, building_id: &str) -> PathBuf {
        self.write_path_for(building_id)
    }

    fn describe(&self) -> MountInfo {
        MountInfo::Local {
            path: self.root.display().to_string(),
        }
    }

    async fn list_snapshots(&self, building_id: &str) -> Result<Vec<SnapshotInfo>> {
        let snap_root = self.asset_root_for(building_id).join("snapshots");
        let mut out = Vec::new();
        let Ok(rd) = tokio::fs::read_dir(&snap_root).await else {
            return Ok(out);
        };
        let mut rd = rd;
        while let Some(entry) = rd.next_entry().await? {
            if !entry.file_type().await?.is_dir() {
                continue;
            }
            let Some(name) = entry.file_name().to_str().map(|s| s.to_string()) else {
                continue;
            };
            if let Some(info) = SnapshotInfo::parse_dir(&name) {
                out.push(info);
            }
        }
        out.sort_by(|a, b| b.created_at.cmp(&a.created_at));
        Ok(out)
    }

    async fn create_snapshot(
        &self,
        building_id: &str,
        snap: &SnapshotInfo,
        yaml: &str,
        assets: &[(String, Bytes)],
    ) -> Result<()> {
        let snap_dir = self
            .asset_root_for(building_id)
            .join("snapshots")
            .join(&snap.dir);
        tokio::fs::create_dir_all(&snap_dir).await?;
        let yaml_path = snap_dir.join(format!("{building_id}.building.yaml"));
        tokio::fs::write(&yaml_path, yaml).await?;
        for (path, bytes) in assets {
            let dest = snap_dir.join(path);
            if let Some(parent) = dest.parent() {
                tokio::fs::create_dir_all(parent).await.ok();
            }
            tokio::fs::write(&dest, &bytes).await?;
        }
        Ok(())
    }

    async fn read_snapshot_yaml(&self, building_id: &str, dir: &str) -> Result<String> {
        let p = self
            .asset_root_for(building_id)
            .join("snapshots")
            .join(dir)
            .join(format!("{building_id}.building.yaml"));
        let resolved = self.check_inside_root(&p)?;
        Ok(tokio::fs::read_to_string(&resolved).await?)
    }

    async fn read_snapshot_asset(
        &self,
        building_id: &str,
        dir: &str,
        path: &str,
    ) -> Result<Bytes> {
        let p = self
            .asset_root_for(building_id)
            .join("snapshots")
            .join(dir)
            .join(path);
        let resolved = self.check_inside_root(&p)?;
        Ok(Bytes::from(tokio::fs::read(&resolved).await?))
    }
}
