// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

use std::future::Future;
use std::path::PathBuf;
use std::sync::Arc;

use anyhow::Result;
use axum::body::Bytes;
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Json, Path, Query, State};
use axum::http::{header, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post, put};
use axum::serve as axum_serve;
use axum::Router;
use futures::{SinkExt, StreamExt};
use serde::Deserialize;
use tokio::net::TcpListener;
use tokio::sync::broadcast;
use tower_http::cors::{Any, CorsLayer};
use tracing::{debug, info, warn};

use crate::state::AppState;
use crate::storage::MountConfig;
use crate::sync;

pub async fn run(
    state: Arc<AppState>,
    port: u16,
    wasm_dir: Option<PathBuf>,
    cache_root: PathBuf,
    fs_browse_root: PathBuf,
    auto_building: Option<String>,
    backend_kind: Option<&'static str>,
    shutdown: impl Future<Output = ()> + Send + 'static,
) -> Result<()> {
    let cors = CorsLayer::new()
        .allow_origin(Any)
        .allow_methods(Any)
        .allow_headers(Any);

    let routed_state = RouteState {
        app: state,
        cache_root: Arc::new(cache_root),
        fs_browse_root: Arc::new(fs_browse_root),
        auto_building: Arc::new(auto_building),
        backend_kind,
    };

    let mut app = Router::new()
        .route("/ws/:room", get(ws_handler))
        .route("/ws", get(ws_handler_no_room))
        .route("/health", get(|| async { "ok" }))
        .route("/config", get(config_handler))
        .route("/mount", post(mount_handler))
        .route("/unmount", post(unmount_handler))
        .route("/mount/status", get(mount_status_handler))
        .route("/buildings", get(buildings_list_handler))
        .route("/buildings/:id/load", post(building_load_handler))
        .route("/buildings/:id/nav_graph/:fleet_id", get(nav_graph_handler))
        .route("/buildings/:id", get(building_get_handler).put(building_put_handler))
        .route(
            "/layer_asset",
            get(layer_asset_handler).put(layer_asset_put_handler),
        )
        .route("/validation_status", get(validation_status_handler))
        .route("/revert_to_last_valid", post(revert_handler))
        .route("/fs/list", get(fs_list_handler))
        .layer(cors)
        .with_state(routed_state);

    if let Some(dir) = wasm_dir {
        info!("serving static files from {}", dir.display());
        let index_path = dir.join("editor_wasm_target.html");
        if !index_path.exists() {
            tracing::error!("editor html not found at {}", index_path.display());
        }
        let index_for_route = index_path.clone();
        app = app.route(
            "/",
            get(move || {
                let p = index_for_route.clone();
                async move {
                    match tokio::fs::read(&p).await {
                        Ok(bytes) => Response::builder()
                            .header(header::CONTENT_TYPE, "text/html")
                            .body(bytes.into())
                            .unwrap(),
                        Err(e) => (
                            StatusCode::NOT_FOUND,
                            format!("editor html not readable at {}: {e}", p.display()),
                        )
                            .into_response(),
                    }
                }
            }),
        );
        let svc = tower_http::services::ServeDir::new(dir.clone())
            .append_index_html_on_directories(false);
        app = app.fallback_service(svc);
    } else {
        tracing::warn!("no --wasm-dir; only API + WebSocket are served");
    }

    let addr = std::net::SocketAddr::from(([0, 0, 0, 0], port));
    let listener = TcpListener::bind(&addr).await?;
    info!("listening on http://{}", listener.local_addr()?);
    info!("WebSocket endpoint: ws://{}/ws", addr);

    axum_serve(listener, app)
        .with_graceful_shutdown(shutdown)
        .await?;
    info!("server stopped");
    Ok(())
}

#[derive(Clone)]
struct RouteState {
    app: Arc<AppState>,
    cache_root: Arc<PathBuf>,
    fs_browse_root: Arc<PathBuf>,
    auto_building: Arc<Option<String>>,
    backend_kind: Option<&'static str>,
}

async fn config_handler(State(rs): State<RouteState>) -> Response {
    let body = serde_json::json!({
        "locked": rs.app.is_locked(),
        "auto_building": rs.auto_building.as_ref().clone(),
        "backend": rs.backend_kind,
    });
    (StatusCode::OK, axum::Json(body)).into_response()
}

async fn mount_handler(State(rs): State<RouteState>, Json(cfg): Json<MountConfig>) -> Response {
    if rs.app.is_locked() {
        return (
            StatusCode::CONFLICT,
            "server is in locked single-mount mode; mount is fixed at startup",
        )
            .into_response();
    }
    match rs.app.mount(cfg, &rs.cache_root).await {
        Ok(buildings) => {
            let body = serde_json::json!({ "buildings": buildings });
            (StatusCode::OK, axum::Json(body)).into_response()
        }
        Err(e) => (StatusCode::BAD_REQUEST, format!("mount failed: {e:#}")).into_response(),
    }
}

async fn unmount_handler(State(rs): State<RouteState>) -> Response {
    if rs.app.is_locked() {
        return (
            StatusCode::CONFLICT,
            "server is in locked single-mount mode; unmount is rejected",
        )
            .into_response();
    }
    match rs.app.unmount().await {
        Ok(()) => (StatusCode::OK, "unmounted").into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("unmount failed: {e:#}"),
        )
            .into_response(),
    }
}

async fn mount_status_handler(State(rs): State<RouteState>) -> Response {
    let status = rs.app.mount_status().await;
    match serde_json::to_string(&status) {
        Ok(body) => Response::builder()
            .header(header::CONTENT_TYPE, "application/json")
            .body(body.into())
            .unwrap(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, format!("serialize: {e}")).into_response(),
    }
}

async fn buildings_list_handler(State(rs): State<RouteState>) -> Response {
    let Some(storage) = rs.app.storage().await else {
        return (StatusCode::CONFLICT, "no backend mounted").into_response();
    };
    match storage.list_buildings().await {
        Ok(list) => (
            StatusCode::OK,
            axum::Json(serde_json::json!({ "buildings": list })),
        )
            .into_response(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("list failed: {e:#}"),
        )
            .into_response(),
    }
}

async fn building_load_handler(Path(id): Path<String>, State(rs): State<RouteState>) -> Response {
    if !id_safe(&id) {
        return (StatusCode::BAD_REQUEST, "invalid id").into_response();
    }
    match rs.app.load_building(&id).await {
        Ok(()) => (StatusCode::OK, "loaded").into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, format!("load failed: {e:#}")).into_response(),
    }
}

async fn nav_graph_handler(
    Path((id, fleet_id)): Path<(String, String)>,
    State(rs): State<RouteState>,
) -> Response {
    if !id_safe(&id) || !id_safe(&fleet_id) {
        return (StatusCode::BAD_REQUEST, "invalid id or fleet_id").into_response();
    }
    match rs.app.nav_graph(&id, &fleet_id).await {
        Ok(yaml) => Response::builder()
            .header(header::CONTENT_TYPE, "application/yaml")
            .body(yaml.into())
            .unwrap(),
        Err(e) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            format!("nav_graph: {e:#}"),
        )
            .into_response(),
    }
}

async fn building_get_handler(
    Path(id): Path<String>,
    State(rs): State<RouteState>,
) -> Response {
    if !id_safe(&id) {
        return (StatusCode::BAD_REQUEST, "invalid id").into_response();
    }
    let Some(storage) = rs.app.storage().await else {
        return (StatusCode::CONFLICT, "no backend mounted").into_response();
    };
    match storage.read_yaml(&id).await {
        Ok(yaml) => Response::builder()
            .header(header::CONTENT_TYPE, "application/yaml")
            .body(yaml.into())
            .unwrap(),
        Err(e) => (StatusCode::NOT_FOUND, format!("read: {e:#}")).into_response(),
    }
}

async fn building_put_handler(
    Path(id): Path<String>,
    State(rs): State<RouteState>,
    body: Bytes,
) -> Response {
    if !id_safe(&id) {
        return (StatusCode::BAD_REQUEST, "invalid id").into_response();
    }
    // Reject empty bodies — they're almost certainly an unintentional wipe
    // (caller forgot to set --data-binary, GET returned 405, etc.).
    if body.is_empty() {
        return (StatusCode::BAD_REQUEST, "empty body rejected").into_response();
    }
    let yaml = match String::from_utf8(body.to_vec()) {
        Ok(s) => s,
        Err(_) => return (StatusCode::BAD_REQUEST, "body must be utf-8 yaml").into_response(),
    };
    match rs.app.replace_building(&id, &yaml).await {
        Ok(()) => (StatusCode::OK, "ok").into_response(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, format!("replace: {e:#}")).into_response(),
    }
}

async fn ws_handler(
    ws: WebSocketUpgrade,
    Path(_room): Path<String>,
    State(rs): State<RouteState>,
) -> Response {
    ws.on_upgrade(move |socket| handle_socket(socket, rs.app))
}

async fn ws_handler_no_room(ws: WebSocketUpgrade, State(rs): State<RouteState>) -> Response {
    ws.on_upgrade(move |socket| handle_socket(socket, rs.app))
}

async fn handle_socket(socket: WebSocket, state: Arc<AppState>) {
    let (mut sink, mut stream) = socket.split();

    let step1 = sync::encode_sync_step1(&state.doc);
    if let Err(e) = sink.send(Message::Binary(step1)).await {
        warn!("ws: failed initial SyncStep1: {e}");
        return;
    }

    let mut bcast: broadcast::Receiver<Vec<u8>> = state.broadcast.subscribe();

    let outbound_state = Arc::clone(&state);
    let outbound = tokio::spawn(async move {
        let _ = outbound_state;
        while let Ok(msg) = bcast.recv().await {
            if sink.send(Message::Binary(msg)).await.is_err() {
                break;
            }
        }
    });

    let inbound_state = Arc::clone(&state);
    let inbound = tokio::spawn(async move {
        while let Some(Ok(msg)) = stream.next().await {
            let bytes = match msg {
                Message::Binary(b) => b,
                Message::Close(_) => break,
                Message::Ping(_) | Message::Pong(_) => continue,
                Message::Text(t) => {
                    debug!("ignoring text WS message ({} bytes)", t.len());
                    continue;
                }
            };
            match sync::handle_message(&inbound_state.doc, &bytes) {
                Ok(result) => {
                    if let Some(reply) = result.reply {
                        let _ = inbound_state.broadcast.send(reply);
                    }
                    if let Some(update) = result.broadcast_update {
                        let msg = sync::encode_update_message(&update);
                        let _ = inbound_state.broadcast.send(msg);
                        inbound_state.mark_dirty().await;
                    }
                }
                Err(e) => warn!("ws: failed to handle message: {e:#}"),
            }
        }
    });

    let _ = tokio::join!(outbound, inbound);
    debug!("ws: connection closed");
}

#[derive(Debug, Deserialize)]
struct LayerAssetQuery {
    id: String,
    path: String,
}

async fn layer_asset_handler(
    Query(q): Query<LayerAssetQuery>,
    State(rs): State<RouteState>,
) -> Response {
    if !id_safe(&q.id) {
        return (StatusCode::BAD_REQUEST, "invalid id").into_response();
    }
    let Some(storage) = rs.app.storage().await else {
        return (StatusCode::CONFLICT, "no backend mounted").into_response();
    };
    match storage.read_asset(&q.id, &q.path).await {
        Ok(bytes) => {
            let mime = mime_from_path(&q.path);
            Response::builder()
                .header(header::CONTENT_TYPE, mime)
                .header(header::ACCESS_CONTROL_ALLOW_ORIGIN, "*")
                .body(bytes.into())
                .unwrap()
        }
        Err(e) => (StatusCode::NOT_FOUND, format!("asset: {e:#}")).into_response(),
    }
}

async fn layer_asset_put_handler(
    Query(q): Query<LayerAssetQuery>,
    State(rs): State<RouteState>,
    body: Bytes,
) -> Response {
    if !id_safe(&q.id) || q.path.is_empty() || q.path.contains("..") {
        return (StatusCode::BAD_REQUEST, "invalid id or path").into_response();
    }
    let Some(storage) = rs.app.storage().await else {
        return (StatusCode::CONFLICT, "no backend mounted").into_response();
    };
    match storage.write_asset(&q.id, &q.path, body).await {
        Ok(()) => (StatusCode::OK, "ok").into_response(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, format!("write: {e:#}")).into_response(),
    }
}

async fn validation_status_handler(State(rs): State<RouteState>) -> Response {
    let status = rs.app.validation_status().await;
    match serde_json::to_string(&status) {
        Ok(body) => Response::builder()
            .header(header::CONTENT_TYPE, "application/json")
            .body(body.into())
            .unwrap(),
        Err(e) => (StatusCode::INTERNAL_SERVER_ERROR, format!("serialize: {e}")).into_response(),
    }
}

async fn revert_handler(State(rs): State<RouteState>) -> Response {
    match rs.app.revert_to_last_valid().await {
        Ok(()) => (StatusCode::OK, "reverted").into_response(),
        Err(e) => (StatusCode::CONFLICT, format!("revert failed: {e:#}")).into_response(),
    }
}

#[derive(Debug, Deserialize)]
struct FsListQuery {
    path: Option<String>,
}

async fn fs_list_handler(Query(q): Query<FsListQuery>, State(rs): State<RouteState>) -> Response {
    let root: &std::path::Path = rs.fs_browse_root.as_path();
    let raw = q.path.unwrap_or_else(|| root.display().to_string());
    let requested = std::path::PathBuf::from(&raw);

    let resolved = match requested.canonicalize() {
        Ok(p) => p,
        Err(e) => {
            return (
                StatusCode::BAD_REQUEST,
                format!("cannot resolve {}: {e}", requested.display()),
            )
                .into_response()
        }
    };
    if !resolved.starts_with(root) {
        return (
            StatusCode::FORBIDDEN,
            format!("path outside browse root ({})", root.display()),
        )
            .into_response();
    }
    if !resolved.is_dir() {
        return (StatusCode::BAD_REQUEST, "not a directory").into_response();
    }

    let mut entries = Vec::new();
    let mut rd = match tokio::fs::read_dir(&resolved).await {
        Ok(r) => r,
        Err(e) => {
            return (
                StatusCode::INTERNAL_SERVER_ERROR,
                format!("readdir failed: {e}"),
            )
                .into_response();
        }
    };
    while let Ok(Some(entry)) = rd.next_entry().await {
        let name = match entry.file_name().into_string() {
            Ok(s) => s,
            Err(_) => continue,
        };
        if name.starts_with('.') {
            continue;
        }
        let ft = match entry.file_type().await {
            Ok(t) => t,
            Err(_) => continue,
        };
        let is_dir = ft.is_dir();
        let is_building_yaml = !is_dir && name.ends_with(".building.yaml");
        entries.push(serde_json::json!({
            "name": name,
            "is_dir": is_dir,
            "is_building_yaml": is_building_yaml,
        }));
    }
    entries.sort_by(|a, b| {
        let ad = a["is_dir"].as_bool().unwrap_or(false);
        let bd = b["is_dir"].as_bool().unwrap_or(false);
        match bd.cmp(&ad) {
            std::cmp::Ordering::Equal => a["name"]
                .as_str()
                .unwrap_or("")
                .cmp(b["name"].as_str().unwrap_or("")),
            other => other,
        }
    });

    let parent = if resolved == *root {
        None
    } else {
        resolved.parent().map(|p| p.display().to_string())
    };
    let body = serde_json::json!({
        "root": root.display().to_string(),
        "path": resolved.display().to_string(),
        "parent": parent,
        "entries": entries,
    });
    Response::builder()
        .header(header::CONTENT_TYPE, "application/json")
        .body(body.to_string().into())
        .unwrap()
}

fn id_safe(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

fn mime_from_path(path: &str) -> &'static str {
    let lower = path.to_ascii_lowercase();
    if lower.ends_with(".png") {
        "image/png"
    } else if lower.ends_with(".jpg") || lower.ends_with(".jpeg") {
        "image/jpeg"
    } else if lower.ends_with(".webp") {
        "image/webp"
    } else if lower.ends_with(".txt") {
        "text/plain"
    } else {
        "application/octet-stream"
    }
}
