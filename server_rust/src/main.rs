// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use clap::{Parser, ValueEnum};
use tokio::signal;
use tracing::info;
use tracing_subscriber::EnvFilter;

use crate::storage::MountConfig;

mod server;
mod state;
mod storage;
mod sync;
mod validate;
mod yaml_bridge;

#[derive(Parser, Debug)]
#[command(name = "imrmf_map_editor")]
#[command(about = "OpenRMF map editor server (Yjs CRDT + storage backend)")]
struct Cli {
    /// HTTP and WebSocket port.
    #[arg(long, default_value_t = 30010)]
    port: u16,

    /// Update interval to backend
    #[arg(long, default_value_t = 2)]
    flush_interval: u64,

    /// Directory of static files served at `/`
    #[arg(long)]
    wasm_dir: Option<PathBuf>,

    /// Root directory for the on-disk yaml cache.
    /// The ROS2 side process watches files under here
    /// Defaults to `$XDG_CACHE_HOME/imrmf_map_editor` or `/var/imrmf/cache`.
    #[arg(long)]
    cache_root: Option<PathBuf>,

    /// Explicit path to the bazelised `building_map_generator_bin` py_binary
    #[arg(long)]
    validator_bin: Option<PathBuf>,

    /// Disable YAML validation regardless of binary discovery.
    #[arg(long)]
    no_validate: bool,

    /// Timeout, in seconds, for a single validation subprocess invocation.
    #[arg(long, default_value_t = 20)]
    validate_timeout: u64,

    /// Root that GET /fs/list is allowed to browse under
    #[arg(long, default_value = "/")]
    fs_browse_root: PathBuf,

    /// Backend to mount at startup.
    /// Non-`none` puts the server in locked single-mount mode (POST /mount and /unmount return 409).
    #[arg(long, value_enum, env = "IMRMF_AUTO_MOUNT_KIND", default_value_t = AutoMountKind::None)]
    auto_mount_kind: AutoMountKind,

    #[arg(long, env = "IMRMF_AUTO_MOUNT_LOCAL_PATH")]
    auto_mount_local_path: Option<String>,

    #[arg(long, env = "IMRMF_S3_BUCKET")]
    s3_bucket: Option<String>,
    #[arg(long, env = "IMRMF_S3_PREFIX", default_value = "")]
    s3_prefix: String,
    #[arg(long, env = "IMRMF_S3_REGION", default_value = "us-east-1")]
    s3_region: String,
    #[arg(long, env = "IMRMF_S3_ENDPOINT")]
    s3_endpoint: Option<String>,
    #[arg(long, env = "IMRMF_S3_ACCESS_KEY_ID")]
    s3_access_key_id: Option<String>,
    #[arg(long, env = "IMRMF_S3_SECRET_ACCESS_KEY")]
    s3_secret_access_key: Option<String>,
    #[arg(long, env = "IMRMF_S3_SESSION_TOKEN")]
    s3_session_token: Option<String>,

    #[arg(long, env = "IMRMF_AUTO_BUILDING")]
    auto_building: Option<String>,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq, ValueEnum)]
enum AutoMountKind {
    None,
    Local,
    S3,
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .with_target(false)
        .init();

    let cli = Cli::parse();

    let cache_root = resolve_cache_root(cli.cache_root.clone())?;
    info!("cache_root = {}", cache_root.display());

    let server_exe = std::env::current_exe().ok();
    let wasm_dir = cli
        .wasm_dir
        .clone()
        .or_else(|| server_exe.as_ref().and_then(|e| wasm_dir_from_runfiles(e)));
    if let Some(d) = wasm_dir.as_ref() {
        info!("wasm_dir  = {}", d.display());
    } else {
        info!("no wasm bundle on disk; serving API + WebSocket only");
    }

    let validator = if cli.no_validate {
        None
    } else {
        let server_exe = std::env::current_exe().ok();
        let (validator_path, parent_runfiles_dir) = if let Some(p) = cli.validator_bin.clone() {
            (Some(p), None)
        } else if let Some((bin, rf)) = server_exe
            .as_ref()
            .and_then(|e| validate::default_binary_from_server_runfiles(e))
        {
            (Some(bin), Some(rf))
        } else {
            (None, None)
        };
        match validator_path {
            Some(path) => {
                let v = validate::Validator {
                    py_binary: path.clone(),
                    runfiles_dir: parent_runfiles_dir,
                    run_timeout: Duration::from_secs(cli.validate_timeout.max(1)),
                };
                match v.precheck() {
                    Ok(()) => {
                        info!("validator: {}", path.display());
                        Some(v)
                    }
                    Err(e) => {
                        tracing::warn!("validator disabled: {e:#}");
                        None
                    }
                }
            }
            None => {
                tracing::info!("validator: not configured");
                None
            }
        }
    };

    let locked = !matches!(cli.auto_mount_kind, AutoMountKind::None);
    let app_state = state::AppState::new(validator, locked);

    let backend_kind: Option<&'static str> = match cli.auto_mount_kind {
        AutoMountKind::None => None,
        AutoMountKind::Local => Some("local"),
        AutoMountKind::S3 => Some("s3"),
    };
    if locked {
        let cfg = build_auto_mount_config(&cli)?;
        let buildings = app_state
            .mount(cfg, &cache_root)
            .await
            .context("auto-mount failed at startup")?;
        info!("auto-mount: {} buildings available", buildings.len());
        if let Some(id) = cli.auto_building.as_ref() {
            if !buildings.iter().any(|b| b == id) {
                tracing::warn!("IMRMF_AUTO_BUILDING={id} not in backend (have {buildings:?})");
            } else {
                app_state
                    .load_building(id)
                    .await
                    .with_context(|| format!("auto-load building {id}"))?;
                info!("auto-load: building '{id}' ready");
            }
        }
    }

    // Periodic flush task
    {
        let app_state = Arc::clone(&app_state);
        let interval = Duration::from_secs(cli.flush_interval.max(1));
        tokio::spawn(async move {
            let mut ticker = tokio::time::interval(interval);
            ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
            loop {
                ticker.tick().await;
                if let Err(e) = app_state.flush_if_dirty().await {
                    tracing::warn!("flush failed: {e:#}");
                }
            }
        });
    }

    let shutdown_state = Arc::clone(&app_state);
    let shutdown = async move {
        match signal::ctrl_c().await {
            Ok(()) => info!("received ctrl-c, flushing and shutting down"),
            Err(e) => tracing::warn!("signal handler failed: {e}"),
        }
        if let Err(e) = shutdown_state.flush_force().await {
            tracing::warn!("final flush failed: {e:#}");
        }
    };

    let fs_browse_root = cli
        .fs_browse_root
        .canonicalize()
        .unwrap_or(cli.fs_browse_root.clone());
    info!("fs_browse_root = {}", fs_browse_root.display());

    server::run(
        app_state,
        cli.port,
        wasm_dir,
        cache_root,
        fs_browse_root,
        cli.auto_building.clone(),
        backend_kind,
        shutdown,
    )
    .await?;
    Ok(())
}

fn build_auto_mount_config(cli: &Cli) -> Result<MountConfig> {
    match cli.auto_mount_kind {
        AutoMountKind::None => unreachable!(),
        AutoMountKind::Local => {
            let path = cli.auto_mount_local_path.clone().ok_or_else(|| {
                anyhow::anyhow!("IMRMF_AUTO_MOUNT_KIND=local requires IMRMF_AUTO_MOUNT_LOCAL_PATH")
            })?;
            Ok(MountConfig::Local { path })
        }
        AutoMountKind::S3 => {
            let bucket = cli.s3_bucket.clone().ok_or_else(|| {
                anyhow::anyhow!("IMRMF_AUTO_MOUNT_KIND=s3 requires IMRMF_S3_BUCKET")
            })?;
            let access_key_id = cli.s3_access_key_id.clone().ok_or_else(|| {
                anyhow::anyhow!("IMRMF_AUTO_MOUNT_KIND=s3 requires IMRMF_S3_ACCESS_KEY_ID")
            })?;
            let secret_access_key = cli.s3_secret_access_key.clone().ok_or_else(|| {
                anyhow::anyhow!("IMRMF_AUTO_MOUNT_KIND=s3 requires IMRMF_S3_SECRET_ACCESS_KEY")
            })?;
            Ok(MountConfig::S3 {
                bucket,
                prefix: cli.s3_prefix.clone(),
                region: cli.s3_region.clone(),
                access_key_id,
                secret_access_key,
                session_token: cli.s3_session_token.clone(),
                endpoint_url: cli.s3_endpoint.clone(),
            })
        }
    }
}

/// Pick a cache root that doesn't require root permissions on a dev machine
fn resolve_cache_root(explicit: Option<PathBuf>) -> Result<PathBuf> {
    let chosen = explicit
        .or_else(|| std::env::var_os("IMRMF_CACHE_ROOT").map(PathBuf::from))
        .or_else(|| {
            std::env::var_os("XDG_CACHE_HOME").map(|p| PathBuf::from(p).join("imrmf_map_editor"))
        })
        .or_else(|| {
            // /var/imrmf/cache is writable inside the docker image. Outside,
            // it usually isn't, so try it but fall back to a per-user dir.
            let candidate = PathBuf::from("/var/imrmf/cache");
            if std::fs::create_dir_all(&candidate).is_ok() {
                Some(candidate)
            } else {
                None
            }
        })
        .or_else(|| {
            std::env::var_os("HOME")
                .map(|h| PathBuf::from(h).join(".cache").join("imrmf_map_editor"))
        })
        .unwrap_or_else(|| std::env::temp_dir().join("imrmf_map_editor"));
    std::fs::create_dir_all(&chosen)
        .with_context(|| format!("creating cache root {}", chosen.display()))?;
    Ok(chosen)
}

fn wasm_dir_from_runfiles(server_exe: &Path) -> Option<PathBuf> {
    let mut runfiles = server_exe.as_os_str().to_owned();
    runfiles.push(".runfiles");
    let runfiles = PathBuf::from(runfiles);
    if !runfiles.is_dir() {
        return None;
    }
    find_dir_with(&runfiles, "editor_wasm_target.html", 6)
}

fn find_dir_with(root: &Path, filename: &str, max_depth: usize) -> Option<PathBuf> {
    if max_depth == 0 {
        return None;
    }
    let entries = std::fs::read_dir(root).ok()?;
    let mut dirs = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if path
            .file_name()
            .and_then(|s| s.to_str())
            .map(|s| s == filename)
            .unwrap_or(false)
        {
            return Some(root.to_path_buf());
        }
        if let Ok(ft) = entry.file_type() {
            if ft.is_dir() || ft.is_symlink() {
                dirs.push(path);
            }
        }
    }
    for d in dirs {
        if let Some(hit) = find_dir_with(&d, filename, max_depth - 1) {
            return Some(hit);
        }
    }
    None
}
