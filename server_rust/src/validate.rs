// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{anyhow, Context, Result};
use tokio::process::Command;
use tokio::time::timeout;

#[derive(Clone, Debug)]
pub struct Validator {
    pub py_binary: PathBuf,
    pub runfiles_dir: Option<PathBuf>,
    pub run_timeout: Duration,
}

impl Validator {
    pub async fn validate(&self, yaml: &str) -> Result<()> {
        self.run_nav(yaml).await.map(|_| ())
    }

    pub async fn generate_nav_graph(&self, yaml: &str, fleet_id: &str) -> Result<String> {
        let tmp = self.run_nav(yaml).await?;
        let path = tmp.path().join("nav_out").join(format!("{fleet_id}.yaml"));
        let content = std::fs::read_to_string(&path)
            .with_context(|| format!("nav graph {} not produced", path.display()))?;
        Ok(content)
    }

    async fn run_nav(&self, yaml: &str) -> Result<tempfile::TempDir> {
        let tmp = tempfile::tempdir().context("creating tempdir")?;
        let yaml_path = tmp.path().join("building.yaml");
        let out_dir = tmp.path().join("nav_out");
        std::fs::write(&yaml_path, yaml)?;
        std::fs::create_dir_all(&out_dir)?;

        let mut cmd;
        if let Some(rf) = self.runfiles_dir.as_ref() {
            cmd = Command::new(&self.py_binary);
            cmd.env("RUNFILES_DIR", rf);
        } else {
            let resolved = canonicalize_or_keep(&self.py_binary);
            cmd = Command::new(&resolved);
            match locate_runfiles(&resolved) {
                Some(RunfilesLocation::Dir(p)) => {
                    cmd.env("RUNFILES_DIR", &p);
                }
                Some(RunfilesLocation::Manifest(p)) => {
                    cmd.env("RUNFILES_MANIFEST_FILE", &p);
                }
                None => {
                    return Err(anyhow!(
                        "validator binary at {} has no companion .runfiles \
                         or .runfiles_manifest, and no parent runfiles_dir \
                         was supplied",
                        resolved.display()
                    ));
                }
            }
        }
        cmd.arg("nav").arg(&yaml_path).arg(&out_dir);
        cmd.env_remove("RUNFILES_MANIFEST_ONLY");
        cmd.env_remove("PYTHON_RUNFILES");
        if self.runfiles_dir.is_some() {
            cmd.env_remove("RUNFILES_MANIFEST_FILE");
        } else {
            cmd.env_remove("RUNFILES_DIR");
        }
        cmd.kill_on_drop(true);

        let output = match timeout(self.run_timeout, cmd.output()).await {
            Ok(Ok(o)) => o,
            Ok(Err(e)) => return Err(anyhow!("failed to spawn {}: {e}", self.py_binary.display())),
            Err(_) => {
                return Err(anyhow!(
                    "{} timed out after {:?}",
                    self.py_binary.display(),
                    self.run_timeout
                ))
            }
        };

        if output.status.success() {
            return Ok(tmp);
        }

        let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
        let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
        let detail = if stderr.is_empty() {
            stdout
        } else if stdout.is_empty() {
            stderr
        } else {
            format!("{stderr}\n---stdout---\n{stdout}")
        };
        Err(anyhow!(
            "building_map_generator nav exited with status {}: {}",
            output.status,
            detail
        ))
    }

    pub fn precheck(&self) -> Result<()> {
        std::fs::symlink_metadata(&self.py_binary)
            .with_context(|| format!("validator binary not found: {}", self.py_binary.display()))?;
        if let Some(rf) = self.runfiles_dir.as_ref() {
            if !rf.is_dir() {
                return Err(anyhow!("validator runfiles_dir missing: {}", rf.display()));
            }
            tracing::info!("validator runfiles (parent tree): {}", rf.display());
            return Ok(());
        }
        let resolved = canonicalize_or_keep(&self.py_binary);
        match locate_runfiles(&resolved) {
            Some(RunfilesLocation::Dir(p)) => {
                tracing::info!("validator runfiles dir: {}", p.display())
            }
            Some(RunfilesLocation::Manifest(p)) => {
                tracing::info!("validator runfiles manifest: {}", p.display())
            }
            None => tracing::warn!(
                "validator: no runfiles alongside {} (py_launcher will fail)",
                resolved.display()
            ),
        }
        Ok(())
    }
}

pub fn default_binary_from_server_runfiles(server_exe: &Path) -> Option<(PathBuf, PathBuf)> {
    find_in_runfiles(server_exe, "building_map_generator_bin")
}

pub fn building_map_server_from_runfiles(server_exe: &Path) -> Option<(PathBuf, PathBuf)> {
    find_in_runfiles(server_exe, "building_map_server_bin")
}

fn find_in_runfiles(server_exe: &Path, name: &str) -> Option<(PathBuf, PathBuf)> {
    let server_runfiles = path_append(server_exe, ".runfiles");
    if !server_runfiles.is_dir() {
        return None;
    }
    let found = walk_for_name(&server_runfiles, name, 5)?;
    Some((found, server_runfiles))
}

fn walk_for_name(root: &Path, name: &str, max_depth: usize) -> Option<PathBuf> {
    if max_depth == 0 {
        return None;
    }
    let entries = std::fs::read_dir(root).ok()?;
    let mut dirs = Vec::new();
    for entry in entries.flatten() {
        let path = entry.path();
        if let Some(fname) = path.file_name().and_then(|s| s.to_str()) {
            if fname == name && std::fs::metadata(&path).is_ok() {
                return Some(path);
            }
        }
        if let Ok(ft) = entry.file_type() {
            if ft.is_dir() {
                dirs.push(path);
            }
        }
    }
    for d in dirs {
        if let Some(hit) = walk_for_name(&d, name, max_depth - 1) {
            return Some(hit);
        }
    }
    None
}

fn canonicalize_or_keep(p: &Path) -> PathBuf {
    std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf())
}

#[derive(Debug)]
enum RunfilesLocation {
    Dir(PathBuf),
    Manifest(PathBuf),
}

fn locate_runfiles(binary: &Path) -> Option<RunfilesLocation> {
    let runfiles_dir = path_append(binary, ".runfiles");
    if runfiles_dir.is_dir() {
        return Some(RunfilesLocation::Dir(runfiles_dir));
    }
    let manifest_file = path_append(binary, ".runfiles_manifest");
    if manifest_file.is_file() {
        return Some(RunfilesLocation::Manifest(manifest_file));
    }
    let inside_manifest = runfiles_dir.join("MANIFEST");
    if inside_manifest.is_file() {
        return Some(RunfilesLocation::Manifest(inside_manifest));
    }
    None
}

fn path_append(p: &Path, suffix: &str) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(suffix);
    PathBuf::from(s)
}
