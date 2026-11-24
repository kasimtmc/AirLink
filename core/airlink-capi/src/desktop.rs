//! This computer's screen, shared with one paired device at a time (phase 4): the frames go on
//! the media stream `desktop`, and the device's `desktop.input` messages are played here.
//! The app decides whether a device may ask (`desktop.start`); this module runs the share and
//! tells the device how it goes (`desktop.started`, `desktop.stopped`).

use std::sync::{Arc, Mutex};

use airlink_core::Node;
use serde_json::{Value, json};

use crate::Shared;

/// The media stream's name.
pub const STREAM: &str = "desktop";

pub struct Running {
    pub peer: String,
    #[cfg(not(target_os = "android"))]
    share: airlink_desktop::Share,
}

pub type Slot = Arc<Mutex<Option<Running>>>;

/// Everything a share's threads need to report back.
#[derive(Clone)]
struct Ctx {
    node: Node,
    rt: tokio::runtime::Handle,
    shared: Arc<Shared>,
    running: Slot,
    peer: String,
}

impl Ctx {
    /// The share ended (or never started): the device and the app hear why.
    fn finish(&self, reason: &str, error: Option<String>) {
        self.node.stop_media(&self.peer, STREAM);
        let mut body = json!({ "reason": reason });
        if let Some(e) = &error {
            body["error"] = json!(e);
        }
        self.shared.push(json!({
            "event": "desktop_stopped", "peer_id": self.peer, "reason": reason, "error": error,
        }));
        let (node, peer) = (self.node.clone(), self.peer.clone());
        self.rt.spawn(async move {
            let _ = node.send_app_message(&peer, "desktop.stopped", body).await;
        });
    }

    fn with_peer(&self, peer: String) -> Self {
        Self {
            peer,
            ..self.clone()
        }
    }
}

#[cfg(not(target_os = "android"))]
impl airlink_desktop::Sink for Ctx {
    fn frame(&self, packet: Vec<u8>) {
        let _enter = self.rt.enter(); // the media writer task is spawned on the node's runtime
        let _ = self.node.send_media(&self.peer, STREAM, packet);
    }

    fn ended(&self, reason: &str) {
        let mine = {
            let mut r = self.running.lock().unwrap();
            if r.as_ref().is_some_and(|r| r.peer == self.peer) {
                r.take()
            } else {
                None
            }
        };
        if let Some(old) = mine {
            drop(old);
            self.finish(reason, None);
        }
    }
}

pub fn supported() -> bool {
    #[cfg(not(target_os = "android"))]
    {
        airlink_desktop::supported()
    }
    #[cfg(target_os = "android")]
    {
        false
    }
}

/// The context of one node's shares.
pub struct Host<'a> {
    pub node: &'a Node,
    pub rt: &'a tokio::runtime::Runtime,
    pub shared: &'a Arc<Shared>,
    pub running: &'a Slot,
}

impl Host<'_> {
    fn ctx(&self, peer: String) -> Ctx {
        Ctx {
            node: self.node.clone(),
            rt: self.rt.handle().clone(),
            shared: self.shared.clone(),
            running: self.running.clone(),
            peer,
        }
    }

    /// Starts sharing with `peer`, replacing a share with another device. `options`:
    /// `{max_width, fps}` (both optional). Result: op_result; the device hears desktop.started
    /// or desktop.stopped.
    pub fn start(&self, op: u64, peer: String, options: Value) {
        let ctx = self.ctx(peer);
        let old = self.running.lock().unwrap().take();
        if let Some(old) = old {
            let previous = ctx.with_peer(old.peer.clone());
            drop(old);
            previous.finish("replaced", None);
        }
        self.rt.spawn(async move {
            let result = run(&ctx, &options).await;
            let ok = result.is_ok();
            match result {
                Ok((width, height)) => {
                    ctx.shared.push(json!({
                        "event": "desktop_started", "peer_id": ctx.peer, "width": width, "height": height,
                    }));
                    let body = json!({ "width": width, "height": height });
                    let _ = ctx.node.send_app_message(&ctx.peer, "desktop.started", body).await;
                }
                Err(e) => {
                    let reason = if supported() { "failed" } else { "unsupported" };
                    ctx.finish(reason, Some(format!("{e:#}")));
                }
            }
            ctx.shared.push(json!({ "event": "op_result", "op": op, "ok": ok, "value": ok }));
        });
    }

    /// Ends the share if `peer` (or anyone, when None) has it; the device hears `reason`.
    pub fn stop(&self, peer: Option<&str>, reason: &str) {
        let taken = {
            let mut r = self.running.lock().unwrap();
            if r.as_ref().is_some_and(|r| peer.is_none_or(|p| p == r.peer)) {
                r.take()
            } else {
                None
            }
        };
        if let Some(old) = taken {
            let ctx = self.ctx(old.peer.clone());
            drop(old);
            ctx.finish(reason, None);
        }
    }
}

#[cfg(not(target_os = "android"))]
async fn run(ctx: &Ctx, options: &Value) -> anyhow::Result<(u32, u32)> {
    let opts = airlink_desktop::Options {
        data_dir: ctx.node.config().data_dir.clone(),
        max_width: options["max_width"]
            .as_u64()
            .map_or(1280, |w| w.clamp(320, 1920) as usize),
        fps: options["fps"]
            .as_u64()
            .map_or(12, |f| f.clamp(1, 30) as u32),
    };
    let share = airlink_desktop::start(opts, Arc::new(ctx.clone())).await?;
    let size = share.size();
    *ctx.running.lock().unwrap() = Some(Running {
        peer: ctx.peer.clone(),
        share,
    });
    Ok(size)
}

#[cfg(target_os = "android")]
async fn run(_ctx: &Ctx, _options: &Value) -> anyhow::Result<(u32, u32)> {
    anyhow::bail!("screen sharing from this device is not available")
}

/// Plays a `desktop.input` body from `peer`, if it has the share. False otherwise.
pub fn input(running: &Slot, peer: &str, body: &str) -> bool {
    #[cfg(not(target_os = "android"))]
    {
        let r = running.lock().unwrap();
        let Some(active) = r.as_ref().filter(|r| r.peer == peer) else {
            return false;
        };
        match airlink_desktop::Input::parse(body) {
            Ok(i) => {
                active.share.input(i);
                true
            }
            Err(_) => false,
        }
    }
    #[cfg(target_os = "android")]
    {
        let _ = (running, peer, body);
        false
    }
}
