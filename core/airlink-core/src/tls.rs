//! TLS configuration. Both sides present self-signed certificates; we accept
//! any certificate at the TLS layer (still verifying handshake signatures, so
//! the peer proves key possession) and authorize by fingerprint afterwards.

use std::sync::Arc;
use std::time::Duration;

use anyhow::{Result, anyhow};
use quinn::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, WebPkiSupportedAlgorithms};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::server::danger::{ClientCertVerified, ClientCertVerifier};
use rustls::{DigitallySignedStruct, DistinguishedName, SignatureScheme};

use crate::identity::{Identity, fingerprint};

pub const ALPN: &[u8] = b"airlink/1";
pub const SERVER_NAME: &str = "airlink";

fn provider() -> Arc<CryptoProvider> {
    Arc::new(rustls::crypto::ring::default_provider())
}

#[derive(Debug)]
struct AcceptAnyCert {
    algs: WebPkiSupportedAlgorithms,
}

impl AcceptAnyCert {
    fn new(p: &CryptoProvider) -> Arc<Self> {
        Arc::new(Self {
            algs: p.signature_verification_algorithms,
        })
    }

    fn verify13(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(message, cert, dss, &self.algs)
    }
}

impl ServerCertVerifier for AcceptAnyCert {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _m: &[u8],
        _c: &CertificateDer<'_>,
        _d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("TLS 1.2 not supported".into()))
    }

    fn verify_tls13_signature(
        &self,
        m: &[u8],
        c: &CertificateDer<'_>,
        d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.verify13(m, c, d)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algs.supported_schemes()
    }
}

impl ClientCertVerifier for AcceptAnyCert {
    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &[]
    }

    fn verify_client_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, rustls::Error> {
        Ok(ClientCertVerified::assertion())
    }

    fn client_auth_mandatory(&self) -> bool {
        true
    }

    fn verify_tls12_signature(
        &self,
        _m: &[u8],
        _c: &CertificateDer<'_>,
        _d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("TLS 1.2 not supported".into()))
    }

    fn verify_tls13_signature(
        &self,
        m: &[u8],
        c: &CertificateDer<'_>,
        d: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.verify13(m, c, d)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algs.supported_schemes()
    }
}

fn transport_config() -> Arc<quinn::TransportConfig> {
    let mut t = quinn::TransportConfig::default();
    t.max_idle_timeout(Some(
        // Short enough that a vanished peer (crash, sleep, Wi-Fi loss) is
        // noticed quickly; keep-alives stop idle-but-alive links from expiring.
        Duration::from_secs(12)
            .try_into()
            .expect("valid idle timeout"),
    ));
    t.keep_alive_interval(Some(Duration::from_secs(4)));
    // Larger windows for high-throughput LAN transfers.
    t.stream_receive_window((16u32 * 1024 * 1024).into());
    t.receive_window((64u32 * 1024 * 1024).into());
    t.send_window(64 * 1024 * 1024);
    Arc::new(t)
}

pub fn server_config(id: &Identity) -> Result<quinn::ServerConfig> {
    let p = provider();
    let mut tls = rustls::ServerConfig::builder_with_provider(p.clone())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .with_client_cert_verifier(AcceptAnyCert::new(&p))
        .with_single_cert(id.cert_chain(), id.private_key())?;
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let mut cfg = quinn::ServerConfig::with_crypto(Arc::new(QuicServerConfig::try_from(tls)?));
    cfg.transport_config(transport_config());
    Ok(cfg)
}

pub fn client_config(id: &Identity) -> Result<quinn::ClientConfig> {
    let p = provider();
    let mut tls = rustls::ClientConfig::builder_with_provider(p.clone())
        .with_protocol_versions(&[&rustls::version::TLS13])?
        .dangerous()
        .with_custom_certificate_verifier(AcceptAnyCert::new(&p))
        .with_client_auth_cert(id.cert_chain(), id.private_key())?;
    tls.alpn_protocols = vec![ALPN.to_vec()];
    let mut cfg = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(tls)?));
    cfg.transport_config(transport_config());
    Ok(cfg)
}

/// Device id of the remote side of an established connection.
pub fn peer_device_id(conn: &quinn::Connection) -> Result<String> {
    let any = conn
        .peer_identity()
        .ok_or_else(|| anyhow!("peer presented no certificate"))?;
    let certs = any
        .downcast::<Vec<CertificateDer<'static>>>()
        .map_err(|_| anyhow!("unexpected peer identity type"))?;
    let leaf = certs
        .first()
        .ok_or_else(|| anyhow!("empty peer certificate chain"))?;
    Ok(fingerprint(leaf))
}
