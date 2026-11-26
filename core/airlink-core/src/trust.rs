//! Persistent list of paired (trusted) devices.

use std::collections::BTreeMap;
use std::fs;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::Result;
use serde::{Deserialize, Serialize};

use crate::protocol::DeviceInfo;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct TrustedPeer {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub last_addr: Option<SocketAddr>,
    /// Unix seconds.
    pub paired_at: u64,
    /// The user marked it trusted ("Güvenilir"): its file offers are accepted without asking
    /// (the app also skips its other prompts and shares the clipboard with it).
    #[serde(default)]
    pub auto_accept: bool,
}

pub struct TrustStore {
    path: PathBuf,
    peers: BTreeMap<String, TrustedPeer>,
}

impl TrustStore {
    pub fn load(path: PathBuf) -> Result<Self> {
        let peers = match fs::read(&path) {
            Ok(bytes) => serde_json::from_slice::<Vec<TrustedPeer>>(&bytes)?
                .into_iter()
                .map(|p| (p.id.clone(), p))
                .collect(),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => BTreeMap::new(),
            Err(e) => return Err(e.into()),
        };
        Ok(Self { path, peers })
    }

    fn save(&self) -> Result<()> {
        if let Some(dir) = self.path.parent() {
            fs::create_dir_all(dir)?;
        }
        let tmp = self.path.with_extension("tmp");
        fs::write(&tmp, serde_json::to_vec_pretty(&self.list())?)?;
        fs::rename(tmp, &self.path)?;
        Ok(())
    }

    pub fn is_trusted(&self, id: &str) -> bool {
        self.peers.contains_key(id)
    }

    pub fn get(&self, id: &str) -> Option<&TrustedPeer> {
        self.peers.get(id)
    }

    pub fn list(&self) -> Vec<TrustedPeer> {
        self.peers.values().cloned().collect()
    }

    pub fn add(&mut self, dev: &DeviceInfo, addr: Option<SocketAddr>) -> Result<()> {
        let paired_at = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let auto_accept = self.peers.get(&dev.id).is_some_and(|p| p.auto_accept);
        self.peers.insert(
            dev.id.clone(),
            TrustedPeer {
                id: dev.id.clone(),
                name: dev.name.clone(),
                platform: dev.platform.clone(),
                last_addr: addr,
                paired_at,
                auto_accept,
            },
        );
        self.save()
    }

    /// Refreshes name/address of an already-trusted peer. No-op otherwise.
    pub fn touch(&mut self, dev: &DeviceInfo, addr: SocketAddr) -> Result<()> {
        let Some(p) = self.peers.get_mut(&dev.id) else {
            return Ok(());
        };
        if p.name == dev.name && p.last_addr == Some(addr) {
            return Ok(());
        }
        p.name = dev.name.clone();
        p.last_addr = Some(addr);
        self.save()
    }

    pub fn auto_accepts(&self, id: &str) -> bool {
        self.peers.get(id).is_some_and(|p| p.auto_accept)
    }

    /// Marks a paired peer trusted (or not). False if it is not paired.
    pub fn set_auto_accept(&mut self, id: &str, on: bool) -> Result<bool> {
        let Some(p) = self.peers.get_mut(id) else {
            return Ok(false);
        };
        if p.auto_accept != on {
            p.auto_accept = on;
            self.save()?;
        }
        Ok(true)
    }

    pub fn remove(&mut self, id: &str) -> Result<bool> {
        let removed = self.peers.remove(id).is_some();
        if removed {
            self.save()?;
        }
        Ok(removed)
    }
}
