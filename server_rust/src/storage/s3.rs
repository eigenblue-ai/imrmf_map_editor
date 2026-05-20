// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::path::PathBuf;

use anyhow::{Context, Result};
use async_trait::async_trait;
use aws_config::{BehaviorVersion, Region};
use aws_credential_types::Credentials;
use aws_sdk_s3::Client;
use bytes::Bytes;

use super::{MountInfo, SnapshotInfo, Storage};

/// Each building lives at `<prefix>/<id>/<id>.building.yaml`
pub struct S3Storage {
    client: Client,
    bucket: String,
    prefix: String,
    region: String,
    cache_root: PathBuf,
}

pub struct S3Config {
    pub bucket: String,
    pub prefix: String,
    pub region: String,
    pub access_key_id: String,
    pub secret_access_key: String,
    pub session_token: Option<String>,
    pub endpoint_url: Option<String>,
    pub cache_root: PathBuf,
}

impl S3Storage {
    pub async fn new(cfg: S3Config) -> Result<Self> {
        let region = Region::new(cfg.region.clone());
        let creds = Credentials::new(
            &cfg.access_key_id,
            &cfg.secret_access_key,
            cfg.session_token.clone(),
            None,
            "imrmf_map_editor",
        );
        let mut loader = aws_config::defaults(BehaviorVersion::latest())
            .region(region)
            .credentials_provider(creds);
        if let Some(url) = cfg.endpoint_url.as_deref() {
            loader = loader.endpoint_url(url);
        }
        let aws_cfg = loader.load().await;
        let mut s3_builder = aws_sdk_s3::config::Builder::from(&aws_cfg);
        if cfg.endpoint_url.is_some() {
            // Path-style addressing makes things work with minio and other S3-compatible endpoints
            s3_builder = s3_builder.force_path_style(true);
        }
        let client = Client::from_conf(s3_builder.build());

        tokio::fs::create_dir_all(&cfg.cache_root)
            .await
            .with_context(|| format!("creating cache root {}", cfg.cache_root.display()))?;

        let store = Self {
            client,
            bucket: cfg.bucket,
            prefix: trim_slashes(&cfg.prefix),
            region: cfg.region,
            cache_root: cfg.cache_root,
        };
        store.head_bucket().await?;
        Ok(store)
    }

    async fn head_bucket(&self) -> Result<()> {
        self.client
            .head_bucket()
            .bucket(&self.bucket)
            .send()
            .await
            .with_context(|| format!("bucket not reachable: {}", self.bucket))?;
        Ok(())
    }

    fn yaml_key(&self, building_id: &str) -> String {
        join_key(
            &self.prefix,
            &format!("{building_id}/{building_id}.building.yaml"),
        )
    }

    fn asset_key(&self, building_id: &str, path: &str) -> String {
        join_key(&self.prefix, &format!("{building_id}/{path}"))
    }

    fn snapshots_prefix(&self, building_id: &str) -> String {
        join_key(&self.prefix, &format!("{building_id}/snapshots/"))
    }

    fn snapshot_yaml_key(&self, building_id: &str, dir: &str) -> String {
        join_key(
            &self.prefix,
            &format!("{building_id}/snapshots/{dir}/{building_id}.building.yaml"),
        )
    }

    fn snapshot_asset_key(&self, building_id: &str, dir: &str, path: &str) -> String {
        join_key(
            &self.prefix,
            &format!("{building_id}/snapshots/{dir}/{path}"),
        )
    }

    fn cache_path(&self, building_id: &str) -> PathBuf {
        self.cache_root
            .join(building_id)
            .join(format!("{building_id}.building.yaml"))
    }
}

#[async_trait]
impl Storage for S3Storage {
    async fn list_buildings(&self) -> Result<Vec<String>> {
        let mut req = self
            .client
            .list_objects_v2()
            .bucket(&self.bucket)
            .delimiter("/");
        let list_prefix = if self.prefix.is_empty() {
            String::new()
        } else {
            format!("{}/", self.prefix)
        };
        if !list_prefix.is_empty() {
            req = req.prefix(&list_prefix);
        }
        let resp = req.send().await.context("s3 list_objects_v2")?;
        let mut out = Vec::new();
        for cp in resp.common_prefixes() {
            let Some(p) = cp.prefix() else { continue };
            let dir = p
                .strip_prefix(&list_prefix)
                .unwrap_or(p)
                .trim_end_matches('/');
            if dir.is_empty() {
                continue;
            }
            // Only list folders that are actual buildings
            let yaml_key = join_key(&self.prefix, &format!("{dir}/{dir}.building.yaml"));
            if self
                .client
                .head_object()
                .bucket(&self.bucket)
                .key(&yaml_key)
                .send()
                .await
                .is_ok()
            {
                out.push(dir.to_string());
            }
        }
        out.sort();
        Ok(out)
    }

    async fn read_yaml(&self, building_id: &str) -> Result<String> {
        let key = self.yaml_key(building_id);
        let resp = self
            .client
            .get_object()
            .bucket(&self.bucket)
            .key(&key)
            .send()
            .await
            .with_context(|| format!("s3 get {}", key))?;
        let bytes = resp.body.collect().await?.into_bytes();
        let content = String::from_utf8(bytes.to_vec())
            .with_context(|| format!("yaml not utf-8: {}", key))?;

        let cache = self.cache_path(building_id);
        if let Some(parent) = cache.parent() {
            tokio::fs::create_dir_all(parent).await.ok();
        }
        tokio::fs::write(&cache, &content).await.ok();
        Ok(content)
    }

    async fn write_yaml(&self, building_id: &str, content: &str) -> Result<()> {
        let key = self.yaml_key(building_id);
        let cache = self.cache_path(building_id);
        if let Some(parent) = cache.parent() {
            tokio::fs::create_dir_all(parent).await.ok();
        }
        let mut tmp = cache.clone();
        tmp.as_mut_os_string().push(".tmp");
        tokio::fs::write(&tmp, content).await?;
        self.client
            .put_object()
            .bucket(&self.bucket)
            .key(&key)
            .body(content.as_bytes().to_vec().into())
            .content_type("text/yaml")
            .send()
            .await
            .with_context(|| format!("s3 put {}", key))?;
        tokio::fs::rename(&tmp, &cache).await?;
        Ok(())
    }

    async fn read_asset(&self, building_id: &str, path: &str) -> Result<Bytes> {
        let key = self.asset_key(building_id, path);
        let resp = self
            .client
            .get_object()
            .bucket(&self.bucket)
            .key(&key)
            .send()
            .await
            .with_context(|| format!("s3 get {}", key))?;
        let bytes = resp.body.collect().await?.into_bytes();
        Ok(bytes)
    }

    async fn write_asset(&self, building_id: &str, path: &str, bytes: Bytes) -> Result<()> {
        let key = self.asset_key(building_id, path);
        self.client
            .put_object()
            .bucket(&self.bucket)
            .key(&key)
            .body(bytes.to_vec().into())
            .send()
            .await
            .with_context(|| format!("s3 put {}", key))?;
        let local = self.cache_root.join(building_id).join(path);
        if let Some(parent) = local.parent() {
            tokio::fs::create_dir_all(parent).await.ok();
        }
        tokio::fs::write(&local, &bytes)
            .await
            .with_context(|| format!("write asset cache {}", local.display()))?;
        Ok(())
    }

    async fn mirror_asset(&self, building_id: &str, path: &str) -> Result<()> {
        let bytes = Storage::read_asset(self, building_id, path).await?;
        let local = self.cache_root.join(building_id).join(path);
        if let Some(parent) = local.parent() {
            tokio::fs::create_dir_all(parent)
                .await
                .with_context(|| format!("create dir {}", parent.display()))?;
        }
        tokio::fs::write(&local, &bytes)
            .await
            .with_context(|| format!("write asset cache {}", local.display()))?;
        Ok(())
    }

    fn local_cache_path(&self, building_id: &str) -> PathBuf {
        self.cache_path(building_id)
    }

    fn describe(&self) -> MountInfo {
        MountInfo::S3 {
            bucket: self.bucket.clone(),
            prefix: self.prefix.clone(),
            region: self.region.clone(),
        }
    }

    async fn list_snapshots(&self, building_id: &str) -> Result<Vec<SnapshotInfo>> {
        let snap_prefix = self.snapshots_prefix(building_id);
        let resp = self
            .client
            .list_objects_v2()
            .bucket(&self.bucket)
            .prefix(&snap_prefix)
            .delimiter("/")
            .send()
            .await
            .context("s3 list_snapshots")?;
        let mut out = Vec::new();
        for cp in resp.common_prefixes() {
            let Some(p) = cp.prefix() else { continue };
            let dir = p.strip_prefix(&snap_prefix).unwrap_or(p).trim_end_matches('/');
            if let Some(info) = SnapshotInfo::parse_dir(dir) {
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
        let yaml_key = self.snapshot_yaml_key(building_id, &snap.dir);
        self.client
            .put_object()
            .bucket(&self.bucket)
            .key(&yaml_key)
            .body(yaml.as_bytes().to_vec().into())
            .content_type("text/yaml")
            .send()
            .await
            .with_context(|| format!("s3 put {}", yaml_key))?;
        for (path, bytes) in assets {
            let key = self.snapshot_asset_key(building_id, &snap.dir, path);
            self.client
                .put_object()
                .bucket(&self.bucket)
                .key(&key)
                .body(bytes.to_vec().into())
                .send()
                .await
                .with_context(|| format!("s3 put {}", key))?;
        }
        Ok(())
    }

    async fn read_snapshot_yaml(&self, building_id: &str, dir: &str) -> Result<String> {
        let key = self.snapshot_yaml_key(building_id, dir);
        let resp = self
            .client
            .get_object()
            .bucket(&self.bucket)
            .key(&key)
            .send()
            .await
            .with_context(|| format!("s3 get {}", key))?;
        let bytes = resp.body.collect().await?.into_bytes();
        String::from_utf8(bytes.to_vec()).with_context(|| format!("yaml not utf-8: {}", key))
    }

    async fn read_snapshot_asset(
        &self,
        building_id: &str,
        dir: &str,
        path: &str,
    ) -> Result<Bytes> {
        let key = self.snapshot_asset_key(building_id, dir, path);
        let resp = self
            .client
            .get_object()
            .bucket(&self.bucket)
            .key(&key)
            .send()
            .await
            .with_context(|| format!("s3 get {}", key))?;
        Ok(resp.body.collect().await?.into_bytes())
    }
}

fn trim_slashes(s: &str) -> String {
    s.trim_matches('/').to_string()
}

fn join_key(prefix: &str, key: &str) -> String {
    if prefix.is_empty() {
        key.to_string()
    } else {
        format!("{prefix}/{key}")
    }
}

// Compile-time guard that S3Storage is what we promise.
#[allow(dead_code)]
fn _is_storage() -> Box<dyn Storage> {
    unreachable!("type guard only");
}
