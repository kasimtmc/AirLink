//! Signing in: OAuth 2 authorization code with PKCE. The browser opens the provider's sign-in
//! page and comes back to `http://localhost:47830/`, where a one-shot listener here takes the
//! code; it is then traded for an access and a refresh token.

use std::time::{Duration, Instant};

use anyhow::{Result, anyhow, bail};
use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use rand::Rng;
use sha2::{Digest, Sha256};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

use crate::{Client, Provider};

/// The redirect's port. Dropbox matches the redirect exactly, port included, so it is fixed; the
/// same address is registered with every provider.
pub const REDIRECT_PORT: u16 = 47830;
pub const REDIRECT_URI: &str = "http://localhost:47830/";

const WAIT: Duration = Duration::from_secs(300);

pub(crate) struct Grant {
    pub access: String,
    pub refresh: Option<String>,
    pub expires: Instant,
}

struct Endpoints {
    authorize: &'static str,
    token: &'static str,
    scope: &'static str,
}

fn endpoints(provider: Provider) -> Endpoints {
    match provider {
        Provider::Onedrive => Endpoints {
            authorize: "https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
            token: "https://login.microsoftonline.com/common/oauth2/v2.0/token",
            scope: "Files.ReadWrite User.Read offline_access",
        },
        Provider::Dropbox => Endpoints {
            authorize: "https://www.dropbox.com/oauth2/authorize",
            token: "https://api.dropboxapi.com/oauth2/token",
            scope: "account_info.read files.metadata.read files.metadata.write files.content.read files.content.write",
        },
        Provider::Gdrive => Endpoints {
            authorize: "https://accounts.google.com/o/oauth2/v2/auth",
            token: "https://oauth2.googleapis.com/token",
            scope: "https://www.googleapis.com/auth/drive",
        },
    }
}

fn random(len: usize) -> String {
    const CHARS: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    let mut rng = rand::rng();
    (0..len).map(|_| CHARS[rng.random_range(0..CHARS.len())] as char).collect()
}

pub(crate) fn challenge(verifier: &str) -> String {
    URL_SAFE_NO_PAD.encode(Sha256::digest(verifier.as_bytes()))
}

fn authorize_url(provider: Provider, client: &Client, state: &str, verifier: &str) -> String {
    let e = endpoints(provider);
    let mut url = url::Url::parse(e.authorize).expect("valid endpoint");
    {
        let mut q = url.query_pairs_mut();
        q.append_pair("client_id", &client.id)
            .append_pair("response_type", "code")
            .append_pair("redirect_uri", REDIRECT_URI)
            .append_pair("scope", e.scope)
            .append_pair("state", state)
            .append_pair("code_challenge", &challenge(verifier))
            .append_pair("code_challenge_method", "S256");
        match provider {
            Provider::Onedrive => {
                q.append_pair("prompt", "select_account");
            }
            Provider::Dropbox => {
                q.append_pair("token_access_type", "offline");
            }
            Provider::Gdrive => {
                // (consent every time: Google gives a refresh token only then)
                q.append_pair("access_type", "offline").append_pair("prompt", "consent");
            }
        }
    }
    url.into()
}

/// Signs in: binds the redirect, hands the sign-in page to `open`, waits for the browser.
pub(crate) async fn sign_in(http: &reqwest::Client, provider: Provider, client: &Client, open: impl FnOnce(String)) -> Result<Grant> {
    let v4 = TcpListener::bind(("127.0.0.1", REDIRECT_PORT)).await;
    let v6 = TcpListener::bind(("::1", REDIRECT_PORT)).await;
    if v4.is_err() && v6.is_err() {
        bail!("giriş için {REDIRECT_PORT} numaralı yerel port açılamadı: başka bir giriş sürüyor olabilir");
    }
    let state = random(32);
    let verifier = random(64);
    open(authorize_url(provider, client, &state, &verifier));
    let code = tokio::time::timeout(WAIT, wait_for_code(v4.ok(), v6.ok(), &state))
        .await
        .map_err(|_| anyhow!("giriş zaman aşımına uğradı"))??;
    let e = endpoints(provider);
    let mut form = vec![
        ("grant_type", "authorization_code".to_owned()),
        ("code", code),
        ("redirect_uri", REDIRECT_URI.to_owned()),
        ("client_id", client.id.clone()),
        ("code_verifier", verifier),
    ];
    if provider == Provider::Onedrive {
        form.push(("scope", e.scope.to_owned()));
    }
    if let Some(secret) = &client.secret {
        form.push(("client_secret", secret.clone()));
    }
    token(http, e.token, &form).await
}

pub(crate) async fn refresh(http: &reqwest::Client, provider: Provider, client: &Client, refresh: &str) -> Result<Grant> {
    let e = endpoints(provider);
    let mut form = vec![
        ("grant_type", "refresh_token".to_owned()),
        ("refresh_token", refresh.to_owned()),
        ("client_id", client.id.clone()),
    ];
    if provider == Provider::Onedrive {
        form.push(("scope", e.scope.to_owned()));
    }
    if let Some(secret) = &client.secret {
        form.push(("client_secret", secret.clone()));
    }
    token(http, e.token, &form).await.map_err(|err| anyhow!("{} oturumu yenilenemedi, hesabı yeniden bağlayın: {err:#}", provider.title()))
}

async fn token(http: &reqwest::Client, url: &str, form: &[(&str, String)]) -> Result<Grant> {
    let response = http.post(url).form(form).send().await?;
    let status = response.status();
    let body = response.text().await.unwrap_or_default();
    if !status.is_success() {
        bail!("{}", crate::http::error_message(status, &body));
    }
    let v: serde_json::Value = serde_json::from_str(&body)?;
    let access = v["access_token"].as_str().ok_or_else(|| anyhow!("yanıtta erişim anahtarı yok"))?.to_owned();
    let expires = Instant::now() + Duration::from_secs(v["expires_in"].as_u64().unwrap_or(3600));
    let refresh = v["refresh_token"].as_str().map(str::to_owned);
    Ok(Grant { access, refresh, expires })
}

/// Serves the redirect until a request carries `state` (and a code or an error).
async fn wait_for_code(v4: Option<TcpListener>, v6: Option<TcpListener>, state: &str) -> Result<String> {
    loop {
        let accepted = tokio::select! {
            r = accept(&v4) => r,
            r = accept(&v6) => r,
        };
        let Ok((mut stream, _)) = accepted else { continue };
        let mut buf = vec![0u8; 8192];
        let mut len = 0;
        // The request line and headers; the body (there is none for a GET) is never read.
        while len < buf.len() {
            match tokio::time::timeout(Duration::from_secs(10), stream.read(&mut buf[len..])).await {
                Ok(Ok(0)) | Err(_) => break,
                Ok(Ok(n)) => {
                    len += n;
                    if buf[..len].windows(4).any(|w| w == b"\r\n\r\n") {
                        break;
                    }
                }
                Ok(Err(_)) => break,
            }
        }
        let request = String::from_utf8_lossy(&buf[..len]);
        let target = request.lines().next().and_then(|l| l.split_whitespace().nth(1)).unwrap_or("");
        let Ok(url) = url::Url::parse(&format!("http://localhost{target}")) else { continue };
        if url.path() != "/" {
            let _ = stream.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").await;
            continue;
        }
        let get = |key: &str| url.query_pairs().find(|(k, _)| k == key).map(|(_, v)| v.into_owned());
        if get("state").as_deref() != Some(state) {
            reply(&mut stream, "Bu giriş isteği AirLink'in değil.").await;
            continue;
        }
        if let Some(code) = get("code") {
            reply(&mut stream, "AirLink hesaba bağlandı. Bu sekmeyi kapatabilirsiniz.").await;
            return Ok(code);
        }
        let error = get("error_description").or_else(|| get("error")).unwrap_or_else(|| "bilinmeyen hata".to_owned());
        reply(&mut stream, "Giriş tamamlanmadı. Bu sekmeyi kapatıp AirLink'te yeniden deneyin.").await;
        bail!("giriş reddedildi: {error}");
    }
}

async fn accept(listener: &Option<TcpListener>) -> std::io::Result<(tokio::net::TcpStream, std::net::SocketAddr)> {
    match listener {
        Some(l) => l.accept().await,
        None => std::future::pending().await,
    }
}

async fn reply(stream: &mut tokio::net::TcpStream, message: &str) {
    let body = format!(
        "<!doctype html><html lang=\"tr\"><meta charset=\"utf-8\"><title>AirLink</title>\
         <body style=\"font-family:sans-serif;display:grid;place-items:center;height:90vh;margin:0\">\
         <p style=\"font-size:1.2rem\">{message}</p></body></html>"
    );
    let head = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    let _ = stream.write_all(head.as_bytes()).await;
    let _ = stream.write_all(body.as_bytes()).await;
    let _ = stream.shutdown().await;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pkce_challenge() {
        // RFC 7636, appendix B.
        assert_eq!(challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    }

    #[test]
    fn sign_in_url() {
        let client = Client { id: "abc".into(), secret: None };
        let url = authorize_url(Provider::Dropbox, &client, "st", "ver");
        assert!(url.starts_with("https://www.dropbox.com/oauth2/authorize?client_id=abc&response_type=code"));
        assert!(url.contains("redirect_uri=http%3A%2F%2Flocalhost%3A47830%2F"));
        assert!(url.contains("token_access_type=offline"));
    }

    #[tokio::test]
    async fn redirect_takes_the_code() {
        let v4 = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let port = v4.local_addr().unwrap().port();
        let wait = tokio::spawn(async move { wait_for_code(Some(v4), None, "s1").await });
        let get = |path: &'static str| async move {
            let mut s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
            s.write_all(format!("GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n").as_bytes()).await.unwrap();
            let mut out = String::new();
            s.read_to_string(&mut out).await.unwrap();
            out
        };
        assert!(get("/favicon.ico").await.starts_with("HTTP/1.1 404"));
        assert!(get("/?code=x&state=other").await.contains("AirLink'in değil"));
        assert!(get("/?code=c0de&state=s1").await.contains("bağlandı"));
        assert_eq!(wait.await.unwrap().unwrap(), "c0de");
    }
}
