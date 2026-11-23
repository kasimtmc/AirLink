//! C ABI over `airlink-core`; see `include/airlink.h` for the contract.
//!
//! Every entry point catches panics (unwinding into C++ is undefined behavior)
//! and never blocks the caller's thread except `airlink_start`/`airlink_stop`.

use std::collections::{HashMap, VecDeque};
use std::ffi::{CStr, CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use airlink_core::{Node, NodeConfig, OutgoingPairing, read_saved_name};
use serde::Deserialize;
use serde_json::{Value, json};

/// Events kept for the app at most; older ones are dropped if it stops polling.
const MAX_QUEUED_EVENTS: usize = 10_000;

pub struct AirlinkNode {
    rt: tokio::runtime::Runtime,
    node: Node,
    shared: Arc<Shared>,
}

struct Shared {
    events: Mutex<VecDeque<String>>,
    pairings: Mutex<HashMap<u64, OutgoingPairing>>,
    next_id: AtomicU64,
}

impl Shared {
    fn push(&self, v: Value) {
        let mut q = self.events.lock().unwrap();
        if q.len() >= MAX_QUEUED_EVENTS {
            q.pop_front();
        }
        q.push_back(v.to_string());
    }

    fn id(&self) -> u64 {
        self.next_id.fetch_add(1, Ordering::Relaxed)
    }

    fn op_result<T: serde::Serialize>(&self, op: u64, res: anyhow::Result<T>) {
        self.push(match res {
            Ok(v) => json!({"event": "op_result", "op": op, "ok": true, "value": v}),
            Err(e) => {
                json!({"event": "op_result", "op": op, "ok": false, "error": format!("{e:#}")})
            }
        });
    }
}

#[derive(Deserialize)]
struct StartConfig {
    /// Defaults to the per-user data directory (as the CLI uses).
    #[serde(default)]
    data_dir: Option<PathBuf>,
    /// Defaults to the user's Downloads/AirLink.
    #[serde(default)]
    download_dir: Option<PathBuf>,
    #[serde(default)]
    device_name: Option<String>,
    #[serde(default)]
    fallback_name: Option<String>,
}

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(fallback)
}

/// # Safety
/// `s` is NULL or a valid NUL-terminated string.
unsafe fn read_str(s: *const c_char) -> Option<String> {
    if s.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(s) }
        .to_str()
        .ok()
        .map(str::to_owned)
}

fn to_c(s: impl Into<Vec<u8>>) -> *mut c_char {
    CString::new(s)
        .map(CString::into_raw)
        .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `node` is NULL or a pointer returned by `airlink_start` and not yet stopped.
unsafe fn node_ref<'a>(node: *mut AirlinkNode) -> Option<&'a AirlinkNode> {
    unsafe { node.as_ref() }
}

fn start(cfg: StartConfig) -> anyhow::Result<AirlinkNode> {
    let rt = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .thread_name("airlink")
        .build()?;
    let mut nc = NodeConfig::with_defaults(cfg.data_dir)?;
    let saved = read_saved_name(&nc.data_dir);
    if let Some(d) = cfg.download_dir {
        nc.download_dir = d;
    }
    let nonempty = |n: &String| !n.trim().is_empty();
    if let Some(n) = cfg.device_name.filter(nonempty) {
        nc.device_name = n;
    } else if let (None, Some(n)) = (saved, cfg.fallback_name.filter(nonempty)) {
        nc.device_name = n;
    }
    let node = rt.block_on(Node::start(nc))?;

    let shared = Arc::new(Shared {
        events: Mutex::new(VecDeque::new()),
        pairings: Mutex::new(HashMap::new()),
        next_id: AtomicU64::new(1),
    });
    let mut rx = node.subscribe();
    let pump = shared.clone();
    rt.spawn(async move {
        use tokio::sync::broadcast::error::RecvError;
        loop {
            match rx.recv().await {
                Ok(ev) => {
                    if let Ok(v) = serde_json::to_value(&ev) {
                        pump.push(v);
                    }
                }
                Err(RecvError::Lagged(_)) => continue,
                Err(RecvError::Closed) => break,
            }
        }
    });
    Ok(AirlinkNode { rt, node, shared })
}

/// # Safety
/// `config_json` is a valid C string; `error_out` is NULL or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_start(
    config_json: *const c_char,
    error_out: *mut *mut c_char,
) -> *mut AirlinkNode {
    // Backtraces are useless here and capturing them has crashed hosts whose
    // bundled unwinders clash with Rust's (seen with Flutter's engine).
    // SAFETY: done before this library starts any thread of its own.
    unsafe {
        std::env::set_var("RUST_BACKTRACE", "0");
        std::env::set_var("RUST_LIB_BACKTRACE", "0");
    }
    let res = catch_unwind(AssertUnwindSafe(|| -> anyhow::Result<AirlinkNode> {
        let raw = unsafe { read_str(config_json) }
            .ok_or_else(|| anyhow::anyhow!("config is not valid UTF-8"))?;
        start(serde_json::from_str(&raw)?)
    }));
    let err = match res {
        Ok(Ok(node)) => return Box::into_raw(Box::new(node)),
        Ok(Err(e)) => format!("{e:#}"),
        Err(_) => "internal error while starting".to_owned(),
    };
    if !error_out.is_null() {
        unsafe { *error_out = to_c(err) };
    }
    std::ptr::null_mut()
}

/// # Safety
/// `node` is NULL or from `airlink_start`; it must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_stop(node: *mut AirlinkNode) {
    if node.is_null() {
        return;
    }
    let n = unsafe { Box::from_raw(node) };
    guard((), move || {
        // Shut down on the runtime's own threads, not by entering the runtime here: this may run
        // while the process exits (a C++ static's destructor), when the calling thread's
        // thread-locals, Tokio's among them, are already gone.
        let AirlinkNode { rt, node, .. } = *n;
        let (tx, rx) = std::sync::mpsc::channel();
        rt.spawn(async move {
            node.shutdown().await;
            let _ = tx.send(());
        });
        let _ = rx.recv_timeout(std::time::Duration::from_secs(3));
        rt.shutdown_background();
    });
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_poll_event(node: *mut AirlinkNode) -> *mut c_char {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return std::ptr::null_mut();
    };
    guard(std::ptr::null_mut(), || {
        match n.shared.events.lock().unwrap().pop_front() {
            Some(s) => to_c(s),
            None => std::ptr::null_mut(),
        }
    })
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_info(node: *mut AirlinkNode) -> *mut c_char {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return std::ptr::null_mut();
    };
    guard(std::ptr::null_mut(), || {
        let i = n.node.info();
        to_c(
            json!({
                "id": i.id, "name": i.name, "platform": i.platform, "port": i.port,
                "download_dir": n.node.config().download_dir.display().to_string(),
            })
            .to_string(),
        )
    })
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_peers(node: *mut AirlinkNode) -> *mut c_char {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return std::ptr::null_mut();
    };
    guard(std::ptr::null_mut(), || {
        let online = n.node.reachable_trusted_peers();
        let discovered = n.node.discovered_peers();
        let mut out: Vec<Value> = n
            .node
            .trusted_peers()
            .into_iter()
            .map(|p| {
                let on = online.contains(&p.id) || discovered.iter().any(|d| d.id == p.id);
                json!({"id": p.id, "name": p.name, "platform": p.platform, "paired": true, "online": on})
            })
            .collect();
        for d in discovered {
            if !out.iter().any(|p| p["id"] == d.id.as_str()) {
                out.push(json!({"id": d.id, "name": d.name, "platform": d.platform, "paired": false, "online": true}));
            }
        }
        let key = |v: &Value| {
            (
                v["name"].as_str().unwrap_or("").to_lowercase(),
                v["id"].to_string(),
            )
        };
        out.sort_by_key(key);
        to_c(Value::Array(out).to_string())
    })
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_respond(
    node: *mut AirlinkNode,
    request_id: u64,
    accept: bool,
) -> bool {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return false;
    };
    guard(false, || n.node.respond(request_id, accept))
}

/// # Safety
/// `node` as for `node_ref`; `peer_id` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_forget(node: *mut AirlinkNode, peer_id: *const c_char) -> bool {
    let (Some(n), Some(id)) = (unsafe { node_ref(node) }, unsafe { read_str(peer_id) }) else {
        return false;
    };
    guard(false, || n.node.forget_peer(&id).unwrap_or(false))
}

/// # Safety
/// `node` as for `node_ref`; `peer_id` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_start_pairing(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
) -> u64 {
    let (Some(n), Some(id)) = (unsafe { node_ref(node) }, unsafe { read_str(peer_id) }) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let (node, shared) = (n.node.clone(), n.shared.clone());
        n.rt.spawn(async move {
            match node.start_pairing_with(&id).await {
                Ok(p) => {
                    let pairing = shared.id();
                    let ev = json!({"event": "pairing_code", "op": op, "pairing": pairing,
                                    "code": p.code(), "peer": p.peer()});
                    shared.pairings.lock().unwrap().insert(pairing, p);
                    shared.push(ev);
                }
                Err(e) => shared.op_result::<()>(op, Err(e)),
            }
        });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_confirm_pairing(
    node: *mut AirlinkNode,
    pairing: u64,
    accept: bool,
) -> u64 {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let shared = n.shared.clone();
        let p = shared.pairings.lock().unwrap().remove(&pairing);
        n.rt.spawn(async move {
            let res = match p {
                Some(p) => p.confirm(accept).await,
                None => Err(anyhow::anyhow!("pairing already finished")),
            };
            shared.op_result(op, res);
        });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_send_files(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    paths_json: *const c_char,
) -> u64 {
    let (Some(n), Some(id), Some(paths)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(paths_json) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let (node, shared) = (n.node.clone(), n.shared.clone());
        n.rt.spawn(async move {
            let res = async {
                let paths: Vec<PathBuf> = serde_json::from_str(&paths)?;
                node.send_files(&id, &paths).await
            }
            .await;
            shared.op_result(op, res);
        });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_send_clipboard(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    text: *const c_char,
) -> u64 {
    let (Some(n), Some(id), Some(text)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(text) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let (node, shared) = (n.node.clone(), n.shared.clone());
        n.rt.spawn(async move {
            let res = node.send_clipboard(&id, &text).await;
            shared.op_result(op, res);
        });
        op
    })
}

/// # Safety
/// `s` is NULL or a string returned by this library, not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}
