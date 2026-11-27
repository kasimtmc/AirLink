//! C ABI over `airlink-core`; see `include/airlink.h` for the contract.
//!
//! Every entry point catches panics (unwinding into C++ is undefined behavior)
//! and never blocks the caller's thread except `airlink_start`/`airlink_stop`.

use std::collections::{HashMap, VecDeque};
use std::ffi::{CStr, CString, c_char};
use std::net::{IpAddr, SocketAddr};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

mod cloud;
mod desktop;

use airlink_core::{DEFAULT_PORT, Node, NodeConfig, OutgoingPairing, read_saved_name};
use openh264::decoder::Decoder;
use openh264::formats::YUVSource;
use serde::Deserialize;
use serde_json::{Value, json};

/// Events kept for the app at most; older ones are dropped if it stops polling.
const MAX_QUEUED_EVENTS: usize = 10_000;

pub struct AirlinkNode {
    rt: tokio::runtime::Runtime,
    node: Node,
    shared: Arc<Shared>,
    desktop: Arc<Mutex<Option<desktop::Running>>>,
    drives: cloud::Drives,
}

pub struct AirlinkH264Decoder {
    decoder: Decoder,
}

/// `airlink_set_prompt_listener`'s callback and its context.
pub type AirlinkPromptFn = extern "C" fn(ctx: *mut std::ffi::c_void, event_json: *const c_char);

#[derive(Clone, Copy)]
struct Listener {
    f: AirlinkPromptFn,
    ctx: usize, // the app's pointer, opaque here
}

/// Events the prompt listener hears: a request appears, or goes away.
const PROMPT_EVENTS: [&str; 4] = [
    "pairing_request",
    "file_offer",
    "request_cancelled",
    "request_answered",
];

struct Shared {
    events: Mutex<VecDeque<String>>,
    pairings: Mutex<HashMap<u64, OutgoingPairing>>,
    next_id: AtomicU64,
    listener: Mutex<Option<Listener>>,
}

impl Shared {
    fn push(&self, v: Value) {
        let text = v.to_string();
        let prompt = v["event"]
            .as_str()
            .is_some_and(|k| PROMPT_EVENTS.contains(&k));
        {
            let mut q = self.events.lock().unwrap();
            if q.len() >= MAX_QUEUED_EVENTS {
                q.pop_front();
            }
            q.push_back(text.clone());
        }
        let listener = *self.listener.lock().unwrap();
        if let (true, Some(l)) = (prompt, listener)
            && let Ok(c) = CString::new(text)
        {
            (l.f)(l.ctx as *mut std::ffi::c_void, c.as_ptr());
        }
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
    let data_dir = nc.data_dir.clone();
    let node = rt.block_on(Node::start(nc))?;

    let shared = Arc::new(Shared {
        events: Mutex::new(VecDeque::new()),
        pairings: Mutex::new(HashMap::new()),
        next_id: AtomicU64::new(1),
        listener: Mutex::new(None),
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
    Ok(AirlinkNode {
        rt,
        node,
        shared,
        desktop: Default::default(),
        drives: cloud::Drives {
            cloud: cloud::open(&data_dir),
            running: Default::default(),
        },
    })
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
        let AirlinkNode {
            rt,
            node,
            desktop,
            drives,
            ..
        } = *n;
        drop(desktop.lock().unwrap().take()); // stops a screen share
        for (_, task) in drives.running.lock().unwrap().drain() {
            task.abort();
        }
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
                "data_dir": n.node.config().data_dir.display().to_string(),
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
                json!({"id": p.id, "name": p.name, "platform": p.platform, "paired": true, "online": on,
                       "auto_accept": p.auto_accept})
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
    guard(false, || {
        let answered = n.node.respond(request_id, accept);
        if answered {
            // So another place showing the same request (a notification, the app) drops it.
            n.shared.push(
                json!({"event": "request_answered", "request_id": request_id, "accepted": accept}),
            );
        }
        answered
    })
}

/// # Safety
/// `node` as for `node_ref`; `f` (if not None) stays callable, with `ctx`, until the node stops.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_set_prompt_listener(
    node: *mut AirlinkNode,
    f: Option<AirlinkPromptFn>,
    ctx: *mut std::ffi::c_void,
) {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return;
    };
    guard((), || {
        *n.shared.listener.lock().unwrap() = f.map(|f| Listener {
            f,
            ctx: ctx as usize,
        });
    })
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

/// Marks a paired device trusted (its file offers no longer ask) or not.
///
/// # Safety
/// `node` as for `node_ref`; `peer_id` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_set_auto_accept(node: *mut AirlinkNode, peer_id: *const c_char, on: bool) -> bool {
    let (Some(n), Some(id)) = (unsafe { node_ref(node) }, unsafe { read_str(peer_id) }) else {
        return false;
    };
    guard(false, || n.node.set_peer_auto_accept(&id, on).unwrap_or(false))
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

/// Pair directly with a numeric IP address, optionally followed by `:port`.
/// This works when multicast discovery cannot cross a VM or routed network.
/// # Safety
/// `node` as for `node_ref`; `address` a valid C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_start_pairing_address(
    node: *mut AirlinkNode,
    address: *const c_char,
) -> u64 {
    let (Some(n), Some(address)) = (unsafe { node_ref(node) }, unsafe { read_str(address) }) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let (node, shared) = (n.node.clone(), n.shared.clone());
        n.rt.spawn(async move {
            let addr = address.parse::<SocketAddr>().ok().or_else(|| {
                address
                    .parse::<IpAddr>()
                    .ok()
                    .map(|ip| SocketAddr::new(ip, DEFAULT_PORT))
            });
            let result = match addr {
                Some(addr) => node.start_pairing(addr).await,
                None => {
                    shared
                        .op_result::<()>(op, Err(anyhow::anyhow!("IP adresi geçersiz: {address}")));
                    return;
                }
            };
            match result {
                Ok(p) => {
                    let pairing = shared.id();
                    shared.push(
                        json!({"event": "pairing_code", "op": op, "pairing": pairing,
                                       "code": p.code(), "peer": p.peer()}),
                    );
                    shared.pairings.lock().unwrap().insert(pairing, p);
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
    unsafe { airlink_send_clipboard_marked(node, peer_id, text, false) }
}

/// As `airlink_send_clipboard`, `sensitive` marking a password.
///
/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_send_clipboard_marked(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    text: *const c_char,
    sensitive: bool,
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
            let res = node.send_clipboard_marked(&id, &text, sensitive).await;
            shared.op_result(op, res);
        });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings (`body_json`: a JSON value).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_send_message(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    kind: *const c_char,
    body_json: *const c_char,
) -> u64 {
    let (Some(n), Some(id), Some(kind), Some(body)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(kind) },
        unsafe { read_str(body_json) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let (node, shared) = (n.node.clone(), n.shared.clone());
        n.rt.spawn(async move {
            let res = async {
                let body: Value = serde_json::from_str(&body)?;
                node.send_app_message(&id, &kind, body).await
            }
            .await;
            shared.op_result(op, res);
        });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings; `data` points to `len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_media_send(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    name: *const c_char,
    data: *const u8,
    len: usize,
) -> bool {
    let (Some(n), Some(id), Some(name)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(name) },
    ) else {
        return false;
    };
    if data.is_null() {
        return false;
    }
    let frame = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
    guard(false, || {
        let _enter = n.rt.enter(); // the writer task is spawned on the node's runtime
        n.node.send_media(&id, &name, frame).is_ok()
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_media_stop(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    name: *const c_char,
) {
    let (Some(n), Some(id), Some(name)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(name) },
    ) else {
        return;
    };
    guard((), || n.node.stop_media(&id, &name))
}

impl AirlinkNode {
    fn desktop(&self) -> desktop::Host<'_> {
        desktop::Host {
            node: &self.node,
            rt: &self.rt,
            shared: &self.shared,
            running: &self.desktop,
        }
    }
}

/// Whether this device can share its own screen with a paired device.
#[unsafe(no_mangle)]
pub extern "C" fn airlink_desktop_supported() -> bool {
    guard(false, desktop::supported)
}

/// Starts sharing this computer's screen with `peer_id` (see airlink.h).
///
/// # Safety
/// `node` as for `node_ref`; strings valid C strings (`options_json`: NULL or a JSON object).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_desktop_start(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    options_json: *const c_char,
) -> u64 {
    let (Some(n), Some(peer)) = (unsafe { node_ref(node) }, unsafe { read_str(peer_id) }) else {
        return 0;
    };
    let options = unsafe { read_str(options_json) }
        .and_then(|o| serde_json::from_str::<Value>(&o).ok())
        .unwrap_or(Value::Null);
    guard(0, || {
        let op = n.shared.id();
        n.desktop().start(op, peer, options);
        op
    })
}

/// Stops sharing this computer's screen with `peer_id` (NULL: with anyone).
///
/// # Safety
/// `node` as for `node_ref`; `peer_id` NULL or a valid C string, `reason` likewise.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_desktop_stop(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    reason: *const c_char,
) {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return;
    };
    let peer = unsafe { read_str(peer_id) };
    let reason = unsafe { read_str(reason) }.unwrap_or_else(|| "computer".into());
    guard((), || n.desktop().stop(peer.as_deref(), &reason))
}

/// Plays a `desktop.input` body from `peer_id`; false if it doesn't have the share or the body
/// is not valid input.
///
/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_desktop_input(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    body_json: *const c_char,
) -> bool {
    let (Some(n), Some(peer), Some(body)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(body_json) },
    ) else {
        return false;
    };
    guard(false, || desktop::input(&n.desktop, &peer, &body))
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings; `out` and `len` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_media_take(
    node: *mut AirlinkNode,
    peer_id: *const c_char,
    name: *const c_char,
    out: *mut *mut u8,
    len: *mut usize,
) -> bool {
    let (Some(n), Some(id), Some(name)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(peer_id) },
        unsafe { read_str(name) },
    ) else {
        return false;
    };
    if out.is_null() || len.is_null() {
        return false;
    }
    guard(false, || match n.node.take_media(&id, &name) {
        Some(frame) => {
            let boxed = frame.into_boxed_slice();
            unsafe {
                *len = boxed.len();
                *out = Box::into_raw(boxed) as *mut u8;
            }
            true
        }
        None => false,
    })
}

/// # Safety
/// `data`/`len` as returned by `airlink_media_take`, not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_free_bytes(data: *mut u8, len: usize) {
    if !data.is_null() {
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, len)) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn airlink_h264_decoder_new() -> *mut AirlinkH264Decoder {
    guard(std::ptr::null_mut(), || match Decoder::new() {
        Ok(decoder) => Box::into_raw(Box::new(AirlinkH264Decoder { decoder })),
        Err(_) => std::ptr::null_mut(),
    })
}

/// # Safety
/// `decoder` is an owned live decoder, `packet` has `len` readable bytes, and output pointers
/// are writable. The returned pixel buffer belongs to `airlink_free_bytes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_h264_decode(
    decoder: *mut AirlinkH264Decoder,
    packet: *const u8,
    len: usize,
    pixels: *mut *mut u8,
    pixels_len: *mut usize,
    width: *mut i32,
    height: *mut i32,
) -> bool {
    if decoder.is_null()
        || packet.is_null()
        || pixels.is_null()
        || pixels_len.is_null()
        || width.is_null()
        || height.is_null()
        || len < 5
        || len > 16 * 1024 * 1024
    {
        return false;
    }
    guard(false, || {
        let bytes = unsafe { std::slice::from_raw_parts(packet, len) };
        if !bytes.starts_with(b"ALH1") {
            return false;
        }
        let Some(image) = (unsafe { &mut *decoder })
            .decoder
            .decode(&bytes[4..])
            .ok()
            .flatten()
        else {
            return false;
        };
        let (w, h) = image.dimensions();
        let Some(size) = w.checked_mul(h).and_then(|n| n.checked_mul(4)) else {
            return false;
        };
        if w == 0 || h == 0 || w > 4096 || h > 4096 || size > 64 * 1024 * 1024 {
            return false;
        }
        let mut rgba = vec![0; size];
        image.write_rgba8(&mut rgba);
        let boxed = rgba.into_boxed_slice();
        unsafe {
            *pixels_len = boxed.len();
            *pixels = Box::into_raw(boxed) as *mut u8;
            *width = w as i32;
            *height = h as i32;
        }
        true
    })
}

/// # Safety
/// `decoder` is null or was returned by `airlink_h264_decoder_new` and not freed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_h264_decoder_free(decoder: *mut AirlinkH264Decoder) {
    if !decoder.is_null() {
        drop(unsafe { Box::from_raw(decoder) });
    }
}

#[cfg(test)]
mod h264_tests {
    use super::*;
    use openh264::encoder::Encoder;
    use openh264::formats::{RgbSliceU8, YUVBuffer};

    #[test]
    fn independent_h264_packet_decodes_to_rgba() {
        let rgb = vec![128_u8; 16 * 16 * 3];
        let yuv = YUVBuffer::from_rgb_source(RgbSliceU8::new(&rgb, (16, 16)));
        let packet = Encoder::new().unwrap().encode(&yuv).unwrap().to_vec();
        let packet = [b"ALH1".as_slice(), packet.as_slice()].concat();
        let decoder = airlink_h264_decoder_new();
        assert!(!decoder.is_null());
        let (mut pixels, mut len, mut width, mut height) = (std::ptr::null_mut(), 0, 0, 0);
        assert!(unsafe {
            airlink_h264_decode(
                decoder,
                packet.as_ptr(),
                packet.len(),
                &mut pixels,
                &mut len,
                &mut width,
                &mut height,
            )
        });
        assert_eq!((width, height, len), (16, 16, 16 * 16 * 4));
        unsafe {
            airlink_free_bytes(pixels, len);
            airlink_h264_decoder_free(decoder);
        }
    }
}

/// Advanced mode (Android): pairs with this device's own wireless debugging. `dir` keeps the key.
/// Result: op_result.
///
/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_adb_pair(
    node: *mut AirlinkNode,
    dir: *const c_char,
    code: *const c_char,
) -> u64 {
    unsafe { airlink_adb_pair_port(node, dir, code, 0) }
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings. `port == 0` discovers via mDNS.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_adb_pair_port(
    node: *mut AirlinkNode,
    dir: *const c_char,
    code: *const c_char,
    port: u16,
) -> u64 {
    let (Some(n), Some(dir), Some(code)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(dir) },
        unsafe { read_str(code) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let shared = n.shared.clone();
        n.rt.spawn(async move {
            shared.op_result(
                op,
                airlink_adb::pair_at(
                    std::path::Path::new(&dir),
                    &code,
                    (port != 0).then_some(port),
                )
                .await,
            );
        });
        op
    })
}

/// Advanced mode: runs `command` as the shell user over this device's wireless debugging.
/// Result: op_result, value = its standard output.
///
/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_adb_shell(
    node: *mut AirlinkNode,
    dir: *const c_char,
    command: *const c_char,
) -> u64 {
    unsafe { airlink_adb_shell_port(node, dir, command, 0) }
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings. `port == 0` discovers via mDNS.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_adb_shell_port(
    node: *mut AirlinkNode,
    dir: *const c_char,
    command: *const c_char,
    port: u16,
) -> u64 {
    let (Some(n), Some(dir), Some(command)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(dir) },
        unsafe { read_str(command) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let shared = n.shared.clone();
        n.rt.spawn(async move {
            shared.op_result(
                op,
                airlink_adb::shell_at(
                    std::path::Path::new(&dir),
                    &command,
                    (port != 0).then_some(port),
                )
                .await,
            );
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

// ---- Windows: the phone's Bluetooth audio (see airlink.h) ------------------------------------

#[cfg(windows)]
use airlink_desktop::bt_audio;

/// Whether Windows has the phone-line transport API (HFP hands-free).
#[cfg(windows)]
#[unsafe(no_mangle)]
pub extern "C" fn airlink_win_call_audio_supported() -> bool {
    guard(false, bt_audio::call_supported)
}

/// Connects to `phone` as its hands-free unit for the current call.
///
/// # Safety
/// `phone` is NULL or a valid NUL-terminated string.
#[cfg(windows)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_win_call_audio_start(phone: *const c_char) {
    let phone = unsafe { read_str(phone) }.unwrap_or_default();
    guard((), || bt_audio::call_start(&phone))
}

#[cfg(windows)]
#[unsafe(no_mangle)]
pub extern "C" fn airlink_win_call_audio_stop() {
    guard((), bt_audio::call_stop)
}

/// The phase (0 off, 1 connecting, 2 waiting for route, 3 active, 4 error); `status_out`
/// (optional) gets the message, to release with airlink_free_string.
///
/// # Safety
/// `status_out` is NULL or a valid pointer.
#[cfg(windows)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_win_call_audio_phase(status_out: *mut *mut c_char) -> i32 {
    let (phase, status) = guard((bt_audio::CallPhase::Error, String::new()), bt_audio::call_status);
    if !status_out.is_null() {
        unsafe { *status_out = to_c(status) };
    }
    phase as i32
}

/// Opens the phone's media stream here (A2DP sink) while `phone` is non-empty.
///
/// # Safety
/// `phone` is NULL or a valid NUL-terminated string.
#[cfg(windows)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_win_player_bluetooth(phone: *const c_char) {
    let phone = unsafe { read_str(phone) }.unwrap_or_default();
    guard((), || bt_audio::player_set_phone(&phone))
}

#[cfg(windows)]
#[unsafe(no_mangle)]
pub extern "C" fn airlink_win_player_bluetooth_status() -> *mut c_char {
    to_c(guard(String::new(), bt_audio::player_status))
}
