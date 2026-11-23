//! API exposed to Dart via flutter_rust_bridge.
//!
//! All networking runs on one dedicated tokio runtime (quinn needs it);
//! async bridge functions just await work spawned there.

use std::path::PathBuf;
use std::sync::{LazyLock, Mutex};

use airlink_core::{Node, NodeConfig, OutgoingPairing};
use anyhow::{Context, Result};
use flutter_rust_bridge::frb;

use crate::frb_generated::StreamSink;

static RT: LazyLock<tokio::runtime::Runtime> = LazyLock::new(|| {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .thread_name("airlink-rt")
        .build()
        .expect("tokio runtime")
});

async fn on_rt<T: Send + 'static>(f: impl std::future::Future<Output = T> + Send + 'static) -> T {
    RT.spawn(f).await.expect("runtime task panicked")
}

#[frb(init)]
pub fn init_app() {
    flutter_rust_bridge::setup_default_user_utils();
}

pub struct DeviceInfoDto {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub port: u16,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PeerDto {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub paired: bool,
    pub online: bool,
}

#[frb(opaque)]
pub struct AirLinkNode {
    node: Node,
}

/// Starts the local node. `device_name` defaults to the saved name or hostname.
pub async fn start_node(
    data_dir: String,
    download_dir: String,
    device_name: Option<String>,
) -> Result<AirLinkNode> {
    on_rt(async move {
        let mut cfg = NodeConfig::with_defaults(Some(PathBuf::from(data_dir)))?;
        cfg.download_dir = PathBuf::from(download_dir);
        if let Some(n) = device_name.filter(|n| !n.trim().is_empty()) {
            cfg.device_name = n;
        }
        if cfg.platform == "linux" && cfg!(target_os = "android") {
            cfg.platform = "android".into();
        }
        let node = Node::start(cfg).await?;
        Ok(AirLinkNode { node })
    })
    .await
}

impl AirLinkNode {
    #[frb(sync)]
    pub fn info(&self) -> DeviceInfoDto {
        let i = self.node.info();
        DeviceInfoDto { id: i.id.clone(), name: i.name.clone(), platform: i.platform.clone(), port: i.port }
    }

    #[frb(sync)]
    pub fn download_dir(&self) -> String {
        self.node.config().download_dir.display().to_string()
    }

    /// Paired devices plus unpaired devices seen on the network.
    #[frb(sync)]
    pub fn peers(&self) -> Vec<PeerDto> {
        let online: Vec<String> = self.node.reachable_trusted_peers();
        let discovered = self.node.discovered_peers();
        let mut out: Vec<PeerDto> = self
            .node
            .trusted_peers()
            .into_iter()
            .map(|p| PeerDto {
                online: online.contains(&p.id) || discovered.iter().any(|d| d.id == p.id),
                id: p.id,
                name: p.name,
                platform: p.platform,
                paired: true,
            })
            .collect();
        for d in discovered {
            if !out.iter().any(|p| p.id == d.id) {
                out.push(PeerDto { id: d.id, name: d.name, platform: d.platform, paired: false, online: true });
            }
        }
        out.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()).then(a.id.cmp(&b.id)));
        out
    }

    /// Node events as JSON objects (see `airlink_core::Event`).
    pub fn events(&self, sink: StreamSink<String>) {
        let mut rx = self.node.subscribe();
        RT.spawn(async move {
            loop {
                match rx.recv().await {
                    Ok(ev) => {
                        let Ok(json) = serde_json::to_string(&ev) else { continue };
                        if sink.add(json).is_err() {
                            break;
                        }
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(_) => break,
                }
            }
        });
    }

    pub async fn start_pairing(&self, peer_id: String) -> Result<PairingSession> {
        let node = self.node.clone();
        let p = on_rt(async move { node.start_pairing_with(&peer_id).await }).await?;
        Ok(PairingSession { code: p.code().to_owned(), peer_name: p.peer().name.clone(), inner: Mutex::new(Some(p)) })
    }

    /// Answers a `pairing_request` or `file_offer` event.
    #[frb(sync)]
    pub fn respond(&self, request_id: u32, accept: bool) -> bool {
        self.node.respond(request_id as u64, accept)
    }

    pub async fn send_files(&self, peer_id: String, paths: Vec<String>) -> Result<String> {
        let node = self.node.clone();
        let paths: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
        on_rt(async move { node.send_files(&peer_id, &paths).await }).await
    }

    pub async fn send_clipboard(&self, peer_id: String, text: String) -> Result<()> {
        let node = self.node.clone();
        on_rt(async move { node.send_clipboard(&peer_id, &text).await }).await
    }

    #[frb(sync)]
    pub fn forget(&self, peer_id: String) -> Result<bool> {
        self.node.forget_peer(&peer_id)
    }

    pub async fn shutdown(&self) {
        let node = self.node.clone();
        on_rt(async move { node.shutdown().await }).await
    }
}

#[frb(opaque)]
pub struct PairingSession {
    code: String,
    peer_name: String,
    inner: Mutex<Option<OutgoingPairing>>,
}

impl PairingSession {
    #[frb(sync)]
    pub fn code(&self) -> String {
        self.code.clone()
    }

    #[frb(sync)]
    pub fn peer_name(&self) -> String {
        self.peer_name.clone()
    }

    /// Sends the local decision; true if both sides accepted.
    pub async fn confirm(&self, accept: bool) -> Result<bool> {
        let p = self.inner.lock().unwrap().take().context("pairing already finished")?;
        on_rt(async move { p.confirm(accept).await }).await
    }
}
