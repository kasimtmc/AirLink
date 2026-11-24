//! AirLink's advanced mode, on the phone: it pairs with its own wireless debugging (adbd) once,
//! as Shizuku does, and then runs commands with the shell's rights (uid 2000), which is what
//! starting the screen helper needs (virtual displays per app, input injection). No computer or
//! `adb` binary takes part.
//!
//! The ports change every time: they are found with mDNS (`_adb-tls-pairing._tcp`, shown while
//! "Pair device with pairing code" is open; `_adb-tls-connect._tcp`, while wireless debugging is on).

use std::net::IpAddr;
use std::path::Path;
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use libadb::keys::rsa::rand_core::OsRng;
use libadb::keys::{AdbKey, KeyName};
use libadb::shell::v2;
use libadb::tls::{TlsClientConfig, TlsIdentity};
use libadb::transport::any;
use libadb::transport::tls::MaybeTls;
use libadb::{Connection, ConnectionConfig, Feature, HostBanner};
use mdns_sd::{ServiceDaemon, ServiceEvent};
use tracing::debug;

type Rt = libadb::transport::runtime::Tokio;
type Usb = libadb::transport::common::NoUsb;

pub const PAIRING_SERVICE: &str = "_adb-tls-pairing._tcp.local.";
pub const CONNECT_SERVICE: &str = "_adb-tls-connect._tcp.local.";

/// The key adbd learns at pairing, kept in `dir` (the app's private storage) as PKCS#8 PEM.
///
/// Stored here rather than with libadb's `keys::store`: that links a temporary file into place
/// (never overwriting a key a device may trust), and Android's SELinux forbids apps hard links in
/// their own data. A new key is written to a temporary file and renamed instead.
fn key(dir: &Path) -> Result<AdbKey> {
    let name = || KeyName::new("airlink@phone").map_err(|e| anyhow!("{e}"));
    let path = dir.join("adbkey");
    if let Ok(pem) = std::fs::read_to_string(&path) {
        return AdbKey::from_pkcs8_pem(&pem, &mut OsRng, name()?)
            .map_err(|e| anyhow!("adb key {}: {e}", path.display()));
    }
    std::fs::create_dir_all(dir)?;
    let key = AdbKey::generate(&mut OsRng, name()?).map_err(|e| anyhow!("adb key: {e}"))?;
    let pem = key.to_pkcs8_pem().map_err(|e| anyhow!("adb key: {e}"))?;
    let tmp = dir.join("adbkey.tmp");
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&tmp)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            f.set_permissions(std::fs::Permissions::from_mode(0o600))?;
        }
        f.write_all(pem.as_bytes())?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, &path)?;
    Ok(key)
}

/// This device's address on its LAN: the one adbd advertises on Wi-Fi.
fn lan_address() -> Option<IpAddr> {
    let s = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect("192.0.2.1:9").ok()?; // no packet is sent: only the route is looked up
    s.local_addr().ok().map(|a| a.ip())
}

/// This device's own port for `service`, found with mDNS within `wait`.
pub async fn discover(service: &str, wait: Duration) -> Result<u16> {
    let service = service.to_owned();
    tokio::task::spawn_blocking(move || -> Result<u16> {
        let ours = lan_address();
        let daemon = ServiceDaemon::new()?;
        let rx = daemon.browse(&service)?;
        let deadline = Instant::now() + wait;
        let mut other: Option<u16> = None; // another device's, kept in case ours can't be told apart
        let found = loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break other;
            }
            match rx.recv_timeout(left) {
                Ok(ServiceEvent::ServiceResolved(s)) => {
                    let mine = s
                        .get_addresses()
                        .iter()
                        .any(|a| Some(a.to_ip_addr()) == ours);
                    debug!(name = %s.fullname, port = s.get_port(), mine, "adb service");
                    if mine {
                        break Some(s.get_port());
                    }
                    other.get_or_insert(s.get_port());
                }
                Ok(_) => {}
                Err(_) => break other,
            }
        };
        let _ = daemon.shutdown();
        found.ok_or_else(|| anyhow!("not found: {service}"))
    })
    .await?
}

/// Pairs with this device's wireless debugging using the six-digit `code` its "Pair device with
/// pairing code" screen shows. Afterwards adbd trusts the key kept in `dir`.
pub async fn pair(dir: &Path, code: &str) -> Result<()> {
    pair_at(dir, code, None).await
}

/// As [`pair`], with an optional port from the six-digit pairing screen when mDNS is unavailable.
pub async fn pair_at(dir: &Path, code: &str, port: Option<u16>) -> Result<()> {
    let code = code.trim();
    if code.len() != 6 || !code.bytes().all(|b| b.is_ascii_digit()) {
        bail!("the pairing code is six digits");
    }
    let key = key(dir)?;
    let port = match port {
        Some(port) if port > 0 => port,
        Some(_) => bail!("pairing port must be between 1 and 65535"),
        None => discover(PAIRING_SERVICE, Duration::from_secs(8))
            .await
            .context(
                "the pairing screen isn't open (Wireless debugging > Pair device with pairing code)",
            )?,
    };
    let identity = TlsIdentity::from_key(&key, &mut OsRng).map_err(|e| anyhow!("tls: {e}"))?;
    let tls = TlsClientConfig::adb(&identity).map_err(|e| anyhow!("tls: {e}"))?;
    let socket = <Rt as libadb::transport::runtime::Runtime>::connect_tcp("127.0.0.1", port)
        .await
        .map_err(|e| anyhow!("connect to the pairing port: {e}"))?;
    let mut transport = MaybeTls::plain(socket);
    libadb::pairing::pair(&mut transport, &tls, &key, code, &mut OsRng)
        .await
        .map_err(|e| anyhow!("pairing failed (a wrong code?): {e}"))?;
    Ok(())
}

/// Whether wireless debugging is on now (its connect port is advertised).
pub async fn available() -> bool {
    discover(CONNECT_SERVICE, Duration::from_secs(3))
        .await
        .is_ok()
}

/// Runs `command` in a shell on this device as adbd's shell user; its standard output.
pub async fn shell(dir: &Path, command: &str) -> Result<String> {
    shell_at(dir, command, None).await
}

/// As [`shell`], with an optional port from Android's Wireless debugging screen when this
/// device does not expose its own `_adb-tls-connect` mDNS record to apps.
pub async fn shell_at(dir: &Path, command: &str, port: Option<u16>) -> Result<String> {
    let key = key(dir)?;
    let port = match port {
        Some(port) if port > 0 => port,
        Some(_) => bail!("wireless debugging port must be between 1 and 65535"),
        None => discover(CONNECT_SERVICE, Duration::from_secs(5))
            .await
            .context("wireless debugging is off")?,
    };
    let identity = TlsIdentity::from_key(&key, &mut OsRng).map_err(|e| anyhow!("tls: {e}"))?;
    let tls = TlsClientConfig::adb(&identity).map_err(|e| anyhow!("tls: {e}"))?;
    let transport = any::connect::<Rt, Usb>(&format!("tcp://127.0.0.1:{port}"))
        .await
        .map_err(|e| anyhow!("connect: {e}"))?;
    let mut conn = Connection::connect_tls(
        transport.tls_ready(),
        key,
        HostBanner::Features(&[Feature::ShellV2]),
        ConnectionConfig::new(),
        &tls,
    )
    .await
    .map_err(|e| anyhow!("adb connect (not paired?): {e}"))?;
    let mut rx = vec![0u8; 256 * 1024];
    let out = v2::exec(&mut conn, command, &mut rx)
        .await
        .map_err(|e| anyhow!("shell: {e}"))?;
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}
