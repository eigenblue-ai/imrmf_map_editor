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
    // None for a local file session: there is a doc and an undo stack, but
    // nothing to broadcast to.
    outbox: Option<UnboundedSender<Vec<u8>>>,
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

/// Uploads one layer asset under the yaml-relative path the map refers to it
/// by. The server re-checks both, this only keeps a bad request off the wire.
#[no_mangle]
pub extern "C" fn imrmf_client_put_asset(
    server_url: *const c_char,
    id: *const c_char,
    path: *const c_char,
    data: *const u8,
    len: usize,
) -> *mut c_char {
    let base = from_c_str(server_url);
    let id = from_c_str(id);
    let path = from_c_str(path);
    if !building_id_is_valid(&id) {
        return err("invalid building id");
    }
    if !asset_path_is_safe(&path) {
        return err("invalid asset path");
    }
    if data.is_null() || len == 0 {
        return err("empty asset");
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    let url = format!(
        "{}/layer_asset?id={}&path={}",
        base.trim_end_matches('/'),
        percent_encode(&id),
        percent_encode(&path)
    );
    match ureq::put(&url)
        .timeout(Duration::from_secs(120))
        .set("content-type", "application/octet-stream")
        .send_bytes(bytes)
    {
        Ok(_) => ok(""),
        Err(e) => err(e),
    }
}

/// Same rule as the server's id_safe. Anything else is rejected there anyway,
/// this just keeps a doomed request off the wire.
fn building_id_is_valid(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

// Relative, no traversal, no drive letters, no control characters. The server
// checks again and the storage backend confines the write to the asset root.
fn asset_path_is_safe(path: &str) -> bool {
    if path.is_empty() || path.len() > 1024 {
        return false;
    }
    if path.starts_with('/') || path.starts_with('\\') || path.contains('\\') {
        return false;
    }
    if path.chars().any(|c| c.is_control()) {
        return false;
    }
    if path.as_bytes().get(1) == Some(&b':') {
        return false;
    }
    !path.split('/').any(|part| part == ".." || part.is_empty())
}

fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.as_bytes() {
        let c = *b as char;
        if c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | '~' | '/') {
            out.push(c);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
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

    install_undo(&doc.lock().unwrap());

    *session().lock().unwrap() = Some(Arc::new(Session {
        doc,
        outbox: Some(tx),
        synced,
        remote_dirty,
    }));
    ok("")
}

// Scoped to our own origin, so undo leaves peers' edits alone. UndoManager is
// neither Send nor Sync, so it lives in a thread local rather than the shared
// Session. Every FFI call comes from the one UI thread.
fn install_undo(doc: &Doc) {
    let root = doc.get_or_insert_map(yaml_bridge::ROOT_KEY);
    let mut opts = UndoOptions::default();
    opts.tracked_origins.insert(Origin::from(LOCAL_ORIGIN));
    UNDO.with(|u| {
        *u.borrow_mut() = Some(UndoManager::<()>::with_scope_and_options(doc, &root, opts))
    });
}

/// A doc with no server behind it, so a map opened from a file gets the same
/// undo stack a mounted one has. The seed is untracked, so the first undo
/// cannot wipe the map the user just opened.
#[no_mangle]
pub extern "C" fn imrmf_client_start_local_session(yaml: *const c_char) -> *mut c_char {
    let yaml = from_c_str(yaml);
    imrmf_client_disconnect();

    let doc = Arc::new(Mutex::new(Doc::new()));
    {
        let d = doc.lock().unwrap();
        if let Err(e) = yaml_bridge::seed_doc(&d, &yaml) {
            return err(e);
        }
        install_undo(&d);
    }

    *session().lock().unwrap() = Some(Arc::new(Session {
        doc,
        outbox: None,
        synced: Arc::new(AtomicBool::new(false)),
        remote_dirty: Arc::new(AtomicBool::new(false)),
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
    if let (Some(tx), false) = (s.outbox.as_ref(), update.is_empty()) {
        let _ = tx.send(sync::encode_update_message(&update));
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
    if let (Some(tx), false) = (s.outbox.as_ref(), update.is_empty()) {
        let _ = tx.send(sync::encode_update_message(&update));
    }
    ok("")
}

/// Whether the outbox is still open, i.e. the sync task is alive.
#[no_mangle]
pub extern "C" fn imrmf_client_is_connected() -> c_int {
    let guard = session().lock().unwrap();
    match guard.as_ref() {
        Some(s) if s.outbox.as_ref().is_some_and(|tx| !tx.is_closed()) => 1,
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    // The session is a process-wide singleton and the undo stack is a thread
    // local, so two tests running at once would drive each other's doc.
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn take(raw: *mut c_char) -> String {
        let out = unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        imrmf_client_string_free(raw);
        out
    }

    fn push(yaml: &str) {
        let c = CString::new(yaml).unwrap();
        assert!(take(imrmf_client_push_yaml(c.as_ptr())).starts_with("OK:"));
    }

    #[test]
    fn building_ids_match_the_server_rule() {
        for good in ["H12", "a", "map_1-2", &"x".repeat(64)] {
            assert!(building_id_is_valid(good), "rejected {good}");
        }
        for bad in [
            "",
            "../etc",
            "a/b",
            "a b",
            "a.b",
            "wörld",
            "a\u{0}b",
            &"x".repeat(65),
        ] {
            assert!(!building_id_is_valid(bad), "accepted {bad:?}");
        }
    }

    #[test]
    fn asset_paths_stay_inside_the_map() {
        for good in ["floor.png", "layers/office.png", "a/b/c.jpg"] {
            assert!(asset_path_is_safe(good), "rejected {good}");
        }
        for bad in [
            "",
            "/etc/passwd",
            "../secrets.png",
            "layers/../../x.png",
            "C:\\win.png",
            "back\\slash.png",
            "nul\u{0}.png",
            "double//slash.png",
        ] {
            assert!(!asset_path_is_safe(bad), "accepted {bad:?}");
        }
    }

    #[test]
    fn query_values_are_encoded() {
        assert_eq!(percent_encode("floor.png"), "floor.png");
        assert_eq!(percent_encode("a b&c=d"), "a%20b%26c%3Dd");
        // Slashes stay: an asset path needs them, and traversal is rejected by
        // asset_path_is_safe rather than encoded into something harmless.
        assert_eq!(percent_encode("layers/x.png"), "layers/x.png");
        assert_eq!(percent_encode("../x"), "../x");
        assert!(!asset_path_is_safe("../x"));
        // The characters that would otherwise end the value or start another.
        assert_eq!(percent_encode("a?b#c"), "a%3Fb%23c");
    }

    // A file session has no socket, so this is the only place the undo stack
    // gets exercised without a server.
    #[test]
    fn local_session_undoes_without_a_server() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let seed = CString::new("name: m\nlevels:\n  L1:\n    elevation: 0\n").unwrap();
        assert!(take(imrmf_client_start_local_session(seed.as_ptr())).starts_with("OK:"));
        assert_eq!(imrmf_client_is_connected(), 0);
        assert_eq!(imrmf_client_can_undo(), 0);

        push("name: m\nlevels:\n  L1:\n    elevation: 7\n");
        assert_eq!(imrmf_client_can_undo(), 1);

        assert_eq!(imrmf_client_undo(), 1);
        let after = take(imrmf_client_snapshot_yaml());
        assert!(
            after.contains("elevation: 0"),
            "undo did not rewind: {after}"
        );
        assert_eq!(
            imrmf_client_remote_dirty(),
            1,
            "UI would never re-read the doc"
        );

        assert_eq!(imrmf_client_redo(), 1);
        let after = take(imrmf_client_snapshot_yaml());
        assert!(
            after.contains("elevation: 7"),
            "redo did not replay: {after}"
        );

        imrmf_client_disconnect();
        assert_eq!(imrmf_client_can_undo(), 0);
    }

    // The seed is what the user just opened. If it were tracked, one undo would
    // empty the document.
    #[test]
    fn seed_is_not_undoable() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let seed = CString::new("name: seeded\nlevels:\n  L1:\n    elevation: 0\n").unwrap();
        assert!(take(imrmf_client_start_local_session(seed.as_ptr())).starts_with("OK:"));
        assert_eq!(imrmf_client_can_undo(), 0);
        assert_eq!(imrmf_client_undo(), 0);
        let still = take(imrmf_client_snapshot_yaml());
        assert!(still.contains("seeded"), "seed was rolled back: {still}");
        imrmf_client_disconnect();
    }
}
