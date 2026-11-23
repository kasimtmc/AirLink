//! LAN discovery over mDNS / DNS-SD (`_airlink._udp.local.`).

use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};

use anyhow::Result;
use mdns_sd::{ServiceDaemon, ServiceEvent, ServiceInfo};
use serde::Serialize;
use tracing::{debug, warn};

use crate::identity::short_id;
use crate::protocol::{DeviceInfo, PROTOCOL_VERSION};

pub const SERVICE_TYPE: &str = "_airlink._udp.local.";

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct DiscoveredPeer {
    pub id: String,
    pub name: String,
    pub platform: String,
    pub addrs: Vec<SocketAddr>,
}

#[derive(Debug, Clone)]
pub enum DiscoveryEvent {
    Found(DiscoveredPeer),
    Lost(String),
}

pub struct Discovery {
    daemon: ServiceDaemon,
}

impl Discovery {
    pub fn start(
        me: &DeviceInfo,
        advertise: bool,
        browse: bool,
        on_event: impl Fn(DiscoveryEvent) + Send + 'static,
    ) -> Result<Self> {
        let daemon = ServiceDaemon::new()?;

        if advertise {
            let host = format!("airlink-{}.local.", short_id(&me.id));
            let props = [
                ("id", me.id.as_str()),
                ("name", me.name.as_str()),
                ("platform", me.platform.as_str()),
                ("v", &PROTOCOL_VERSION.to_string()),
            ];
            let info = ServiceInfo::new(
                SERVICE_TYPE,
                short_id(&me.id),
                &host,
                "",
                me.port,
                &props[..],
            )?
            .enable_addr_auto();
            daemon.register(info)?;
        }

        if browse {
            let rx = daemon.browse(SERVICE_TYPE)?;
            let my_id = me.id.clone();
            std::thread::Builder::new()
                .name("airlink-mdns".into())
                .spawn(move || {
                    // fullname -> device id, to translate removals.
                    let mut names: HashMap<String, String> = HashMap::new();
                    while let Ok(ev) = rx.recv() {
                        match ev {
                            ServiceEvent::ServiceResolved(svc) => {
                                let Some(id) = svc.get_property_val_str("id").map(str::to_owned)
                                else {
                                    continue;
                                };
                                if id == my_id {
                                    continue;
                                }
                                let port = svc.get_port();
                                let mut addrs: Vec<SocketAddr> = svc
                                    .get_addresses()
                                    .iter()
                                    .map(|ip| ip.to_ip_addr())
                                    .filter(|ip| matches!(ip, IpAddr::V4(_)))
                                    .map(|ip| SocketAddr::new(ip, port))
                                    .collect();
                                // Real LAN addresses first, loopback last.
                                addrs.sort_by_key(|a| (a.ip().is_loopback(), *a));
                                if addrs.is_empty() {
                                    continue;
                                }
                                names.insert(svc.fullname.clone(), id.clone());
                                let peer = DiscoveredPeer {
                                    name: svc
                                        .get_property_val_str("name")
                                        .unwrap_or("?")
                                        .to_owned(),
                                    platform: svc
                                        .get_property_val_str("platform")
                                        .unwrap_or("?")
                                        .to_owned(),
                                    id,
                                    addrs,
                                };
                                debug!(?peer, "mdns resolved");
                                on_event(DiscoveryEvent::Found(peer));
                            }
                            ServiceEvent::ServiceRemoved(_, fullname) => {
                                if let Some(id) = names.remove(&fullname) {
                                    on_event(DiscoveryEvent::Lost(id));
                                }
                            }
                            ServiceEvent::SearchStopped(_) => break,
                            _ => {}
                        }
                    }
                })?;
        }

        Ok(Self { daemon })
    }

    pub fn shutdown(&self) {
        if let Err(e) = self.daemon.shutdown() {
            warn!("mdns shutdown: {e}");
        }
    }
}
