//! Long-term device identity: a self-signed certificate whose SHA-256
//! fingerprint is the device id.

use std::fs;
use std::path::Path;

use anyhow::{Context, Result};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use sha2::{Digest, Sha256};

const CERT_FILE: &str = "identity.cert.der";
const KEY_FILE: &str = "identity.key.der";

#[derive(Clone)]
pub struct Identity {
    cert_der: Vec<u8>,
    key_der: Vec<u8>,
    device_id: String,
}

impl Identity {
    /// Loads the identity from `dir`, generating and persisting a new one if absent.
    pub fn load_or_create(dir: &Path) -> Result<Self> {
        let cert_path = dir.join(CERT_FILE);
        let key_path = dir.join(KEY_FILE);
        if cert_path.exists() && key_path.exists() {
            let cert_der = fs::read(&cert_path).context("reading identity certificate")?;
            let key_der = fs::read(&key_path).context("reading identity key")?;
            return Ok(Self::from_der(cert_der, key_der));
        }
        let id = Self::generate()?;
        fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
        write_private(&key_path, &id.key_der)?;
        fs::write(&cert_path, &id.cert_der)?;
        Ok(id)
    }

    /// Generates a fresh, non-persisted identity.
    pub fn generate() -> Result<Self> {
        let ck = rcgen::generate_simple_self_signed(vec!["airlink".to_string()])?;
        Ok(Self::from_der(
            ck.cert.der().to_vec(),
            ck.signing_key.serialize_der(),
        ))
    }

    fn from_der(cert_der: Vec<u8>, key_der: Vec<u8>) -> Self {
        let device_id = fingerprint(&cert_der);
        Self {
            cert_der,
            key_der,
            device_id,
        }
    }

    pub fn device_id(&self) -> &str {
        &self.device_id
    }

    pub fn cert_chain(&self) -> Vec<CertificateDer<'static>> {
        vec![CertificateDer::from(self.cert_der.clone())]
    }

    pub fn private_key(&self) -> PrivateKeyDer<'static> {
        PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(self.key_der.clone()))
    }
}

/// SHA-256 of a DER certificate, lowercase hex.
pub fn fingerprint(cert_der: &[u8]) -> String {
    hex::encode(Sha256::digest(cert_der))
}

/// First 8 characters of a device id, for display.
pub fn short_id(id: &str) -> &str {
    &id[..id.len().min(8)]
}

#[cfg(unix)]
fn write_private(path: &Path, data: &[u8]) -> Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    let mut f = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(path)?;
    f.write_all(data)?;
    Ok(())
}

#[cfg(not(unix))]
fn write_private(path: &Path, data: &[u8]) -> Result<()> {
    fs::write(path, data)?;
    Ok(())
}
