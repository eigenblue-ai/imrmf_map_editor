// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The ImRmfMapEditor Authors

//! Desktop CRDT client as a C ABI. The browser gets all of this from JS.
//! Local and S3 are mount configs the server owns, so this only does mount,
//! load, and sync.

use std::cell::RefCell;
use std::ffi::{c_char, c_int, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use imrmf_core::{sync, yaml_bridge};
use tokio::runtime::Runtime;
use tokio::sync::mpsc::{self, UnboundedSender};
use tokio_tungstenite::tungstenite::Message;
use yrs::undo::{Options as UndoOptions, UndoManager};
use yrs::{Doc, Origin, ReadTxn, Transact};

// Tags transactions this client makes, so undo only rewinds our own edits.
const LOCAL_ORIGIN: &[u8] = b"imrmf-local";

thread_local! {
    static UNDO: RefCell<Option<UndoManager<()>>> = const { RefCell::new(None) };
}

struct Session {
    doc: Arc<Mutex<Doc>>,
    outbox: UnboundedSender<Vec<u8>>,
    synced: Arc<AtomicBool>,
    remote_dirty: Arc<AtomicBool>,
}

fn runtime() -> &'static Runtime {
    static RT: OnceLock<Runtime> = OnceLock::new();
    RT.get_or_init(|| Runtime::new().expect("failed to start the client's tokio runtime"))
}

fn session() -> &'static Mutex<Option<Arc<Session>>> {
    static S: OnceLock<Mutex<Option<Arc<Session>>>> = OnceLock::new();
    S.get_or_init(|| Mutex::new(None))
}

fn to_c_string(s: String) -> *mut c_char {
    CString::new(s)
        .unwrap_or_else(|_| CString::new("ERR:string contained a nul byte").unwrap())
        .into_raw()
}

fn from_c_str(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

fn ok(payload: impl Into<String>) -> *mut c_char {
    to_c_string(format!("OK:{}", payload.into()))
}

fn err(message: impl std::fmt::Display) -> *mut c_char {
    to_c_string(format!("ERR:{message}"))
}

/// Frees any string returned by this library.
#[no_mangle]
pub extern "C" fn imrmf_client_string_free(p: *mut c_char) {
    if !p.is_null() {
        unsafe { drop(CString::from_raw(p)) };
    }
}

// ---------------------------------------------------------------- REST

fn post_json(url: &str, body: serde_json::Value) -> Result<String, String> {
    ureq::post(url)
        .timeout(Duration::from_secs(30))
        .send_json(body)
        .map_err(|e| e.to_string())?
        .into_string()
        .map_err(|e| e.to_string())
}

fn get_text(url: &str) -> Result<String, String> {
    ureq::get(url)
        .timeout(Duration::from_secs(30))
        .call()
        .map_err(|e| e.to_string())?
        .into_string()
        .map_err(|e| e.to_string())
}

/// GET /config, handed back verbatim. Credentials are never in it, only whether
/// the server holds any.
#[no_mangle]
pub extern "C" fn imrmf_client_fetch_config(server_url: *const c_char) -> *mut c_char {
    let base = from_c_str(server_url);
    match get_text(&format!("{}/config", base.trim_end_matches('/'))) {
        Ok(body) => ok(body),
        Err(e) => err(e),
    }
}

/// GET /buildings for a server that already has a backend mounted, so a locked
/// or auto-mounted server can be joined without POSTing a mount.
#[no_mangle]
pub extern "C" fn imrmf_client_list_buildings(server_url: *const c_char) -> *mut c_char {
    let base = from_c_str(server_url);
    match get_text(&format!("{}/buildings", base.trim_end_matches('/'))) {
        Ok(body) => match serde_json::from_str::<serde_json::Value>(&body) {
            Ok(v) => {
                let ids: Vec<String> = v["buildings"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(|x| x.as_str().map(|s| s.to_string()))
                            .collect()
                    })
                    .unwrap_or_default();
                ok(ids.join("\n"))
            }
            Err(e) => err(format!("buildings response: {e}")),
        },
        Err(e) => err(e),
    }
}

/// Mounts a backend. `config_json` is the server's MountConfig.
#[no_mangle]
pub extern "C" fn imrmf_client_mount(
    server_url: *const c_char,
    config_json: *const c_char,
) -> *mut c_char {
    let base = from_c_str(server_url);
    let raw = from_c_str(config_json);
    let cfg: serde_json::Value = match serde_json::from_str(&raw) {
        Ok(v) => v,
        Err(e) => return err(format!("bad mount config: {e}")),
    };
    match post_json(&format!("{}/mount", base.trim_end_matches('/')), cfg) {
        // Hand back newline-separated ids so the caller needs no JSON parser.
        Ok(body) => match serde_json::from_str::<serde_json::Value>(&body) {
            Ok(v) => {
                let ids: Vec<String> = v["buildings"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(|x| x.as_str().map(|s| s.to_string()))
                            .collect()
                    })
                    .unwrap_or_default();
                ok(ids.join("\n"))
            }
            Err(e) => err(format!("mount response: {e}")),
        },
        Err(e) => err(e),
    }
}

/// Tells the server to load a building into the shared doc.
#[no_mangle]
pub extern "C" fn imrmf_client_load_building(
    server_url: *const c_char,
    id: *const c_char,
) -> *mut c_char {
    let base = from_c_str(server_url);
    let id = from_c_str(id);
    let url = format!("{}/buildings/{}/load", base.trim_end_matches('/'), id);
    match ureq::post(&url)
        .timeout(Duration::from_secs(60))
        .call()
        .map_err(|e| e.to_string())
    {
        Ok(_) => ok(""),
        Err(e) => err(e),
    }
}

/// Writes a building to the mounted backend, creating it if it is new.
#[no_mangle]
pub extern "C" fn imrmf_client_put_building(
    server_url: *const c_char,
    id: *const c_char,
    yaml: *const c_char,
) -> *mut c_char {
    let base = from_c_str(server_url);
    let id = from_c_str(id);
    let yaml = from_c_str(yaml);
    let url = format!("{}/buildings/{}", base.trim_end_matches('/'), id);
    match ureq::put(&url)
        .timeout(Duration::from_secs(60))
        .set("content-type", "application/yaml")
        .send_string(&yaml)
    {
        Ok(_) => ok(""),
        Err(e) => err(e),
    }
}

// ---------------------------------------------------------------- sync

/// Connects to the y-sync WebSocket and syncs in the background.
#[no_mangle]
pub extern "C" fn imrmf_client_connect(ws_url: *const c_char) -> *mut c_char {
    let url = from_c_str(ws_url);
    imrmf_client_disconnect();

    let doc = Arc::new(Mutex::new(Doc::new()));
    let synced = Arc::new(AtomicBool::new(false));
    let remote_dirty = Arc::new(AtomicBool::new(false));
    let (tx, mut rx) = mpsc::unbounded_channel::<Vec<u8>>();

    let task_doc = Arc::clone(&doc);
    let task_synced = Arc::clone(&synced);
    let task_dirty = Arc::clone(&remote_dirty);
    let connect_url = url.clone();

    runtime().spawn(async move {
        let (stream, _) = match tokio_tungstenite::connect_async(&connect_url).await {
            Ok(pair) => pair,
            Err(e) => {
                tracing_log(format!("ws connect failed: {e}"));
                return;
            }
        };
        let (mut sink, mut source) = stream.split();

        // Announce what we have. The server answers with its own step1 too.
        let step1 = {
            let doc = task_doc.lock().unwrap();
            sync::encode_sync_step1(&doc)
        };
        if sink.send(Message::Binary(step1)).await.is_err() {
            return;
        }

        loop {
            tokio::select! {
                outgoing = rx.recv() => {
                    let Some(bytes) = outgoing else { break };
                    if sink.send(Message::Binary(bytes)).await.is_err() {
                        break;
                    }
                }
                incoming = source.next() => {
                    let Some(Ok(msg)) = incoming else { break };
                    let Message::Binary(bytes) = msg else { continue };

                    let reply = {
                        let doc = task_doc.lock().unwrap();
                        match sync::handle_message(&doc, &bytes) {
                            Ok(r) => {
                                if r.broadcast_update.is_some() {
                                    task_dirty.store(true, Ordering::SeqCst);
                                }
                                task_synced.store(true, Ordering::SeqCst);
                                r.reply
                            }
                            Err(e) => {
                                tracing_log(format!("sync: {e}"));
                                None
                            }
                        }
                    };
                    if let Some(reply) = reply {
                        if sink.send(Message::Binary(reply)).await.is_err() {
                            break;
                        }
                    }
                }
            }
        }
        task_synced.store(false, Ordering::SeqCst);
    });

    // Scoped to our own origin, so undo leaves peers' edits alone. UndoManager
    // is neither Send nor Sync, so it lives in a thread local rather than the
    // shared Session. Every FFI call comes from the one UI thread.
    {
        let d = doc.lock().unwrap();
        let root = d.get_or_insert_map(yaml_bridge::ROOT_KEY);
        let mut opts = UndoOptions::default();
        opts.tracked_origins.insert(Origin::from(LOCAL_ORIGIN));
        let mgr = UndoManager::<()>::with_scope_and_options(&d, &root, opts);
        UNDO.with(|u| *u.borrow_mut() = Some(mgr));
    }

    *session().lock().unwrap() = Some(Arc::new(Session {
        doc,
        outbox: tx,
        synced,
        remote_dirty,
    }));
    ok("")
}

/// Undo/redo over the local edit stack. The change is broadcast like any other
/// edit, and remote_dirty is raised so the UI re-reads the doc.
fn undo_step(redo: bool) -> bool {
    let guard = session().lock().unwrap();
    let Some(s) = guard.as_ref() else {
        return false;
    };
    let doc = s.doc.lock().unwrap();
    let before = doc.transact().state_vector();
    let changed = UNDO.with(|u| match u.borrow_mut().as_mut() {
        Some(mgr) => {
            if redo {
                mgr.redo_blocking()
            } else {
                mgr.undo_blocking()
            }
        }
        None => false,
    });
    if !changed {
        return false;
    }
    let update = doc.transact().encode_state_as_update_v1(&before);
    if !update.is_empty() {
        let _ = s.outbox.send(sync::encode_update_message(&update));
    }
    s.remote_dirty.store(true, Ordering::SeqCst);
    true
}

#[no_mangle]
pub extern "C" fn imrmf_client_undo() -> c_int {
    undo_step(false) as c_int
}

#[no_mangle]
pub extern "C" fn imrmf_client_redo() -> c_int {
    undo_step(true) as c_int
}

#[no_mangle]
pub extern "C" fn imrmf_client_can_undo() -> c_int {
    UNDO.with(|u| u.borrow().as_ref().map(|m| m.can_undo()).unwrap_or(false)) as c_int
}

#[no_mangle]
pub extern "C" fn imrmf_client_can_redo() -> c_int {
    UNDO.with(|u| u.borrow().as_ref().map(|m| m.can_redo()).unwrap_or(false)) as c_int
}

fn tracing_log(msg: String) {
    eprintln!("[imrmf client] {msg}");
}

#[no_mangle]
pub extern "C" fn imrmf_client_disconnect() {
    UNDO.with(|u| *u.borrow_mut() = None);
    *session().lock().unwrap() = None;
}

#[no_mangle]
pub extern "C" fn imrmf_client_is_synced() -> c_int {
    let guard = session().lock().unwrap();
    match guard.as_ref() {
        Some(s) if s.synced.load(Ordering::SeqCst) => 1,
        _ => 0,
    }
}

#[no_mangle]
pub extern "C" fn imrmf_client_remote_dirty() -> c_int {
    let guard = session().lock().unwrap();
    match guard.as_ref() {
        Some(s) if s.remote_dirty.load(Ordering::SeqCst) => 1,
        _ => 0,
    }
}

#[no_mangle]
pub extern "C" fn imrmf_client_clear_remote_dirty() {
    if let Some(s) = session().lock().unwrap().as_ref() {
        s.remote_dirty.store(false, Ordering::SeqCst);
    }
}

/// The doc as building.yaml. Empty string until the first sync lands.
#[no_mangle]
pub extern "C" fn imrmf_client_snapshot_yaml() -> *mut c_char {
    let guard = session().lock().unwrap();
    let Some(s) = guard.as_ref() else {
        return to_c_string(String::new());
    };
    let doc = s.doc.lock().unwrap();
    let txn = doc.transact();
    match yaml_bridge::serialize_doc(&txn) {
        Ok(yaml) => to_c_string(yaml),
        Err(_) => to_c_string(String::new()),
    }
}

/// Replaces the doc contents with `yaml` and ships the resulting update.
#[no_mangle]
pub extern "C" fn imrmf_client_push_yaml(yaml: *const c_char) -> *mut c_char {
    let yaml = from_c_str(yaml);
    let guard = session().lock().unwrap();
    let Some(s) = guard.as_ref() else {
        return err("not connected");
    };
    let doc = s.doc.lock().unwrap();

    // Diff against the pre-edit state vector, so only this edit goes out.
    let before = doc.transact().state_vector();
    if let Err(e) = yaml_bridge::seed_doc_with_origin(&doc, &yaml, LOCAL_ORIGIN) {
        return err(e);
    }
    let update = doc.transact().encode_state_as_update_v1(&before);
    if !update.is_empty() {
        let _ = s.outbox.send(sync::encode_update_message(&update));
    }
    ok("")
}

/// Whether the outbox is still open, i.e. the sync task is alive.
#[no_mangle]
pub extern "C" fn imrmf_client_is_connected() -> c_int {
    let guard = session().lock().unwrap();
    match guard.as_ref() {
        Some(s) if !s.outbox.is_closed() => 1,
        _ => 0,
    }
}
