//! Updates: the latest AirLink release on GitHub (kasimtmc/AirLink), and its files downloaded
//! with their SHA-256 checked against the release's SHA256SUMS. What to do with a file (replace
//! the AppImage, run the installer, open the APK...) is the app's: see `airlink_update_*` in
//! airlink.h.

use std::ffi::c_char;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, Once};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use futures_util::StreamExt;
use serde::Serialize;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use tokio::io::AsyncWriteExt;

use crate::{AirlinkNode, Shared, guard, node_ref, read_str};

const LATEST: &str = "https://api.github.com/repos/kasimtmc/AirLink/releases/latest";

#[derive(Serialize)]
struct Release {
    version: String,
    tag: String,
    notes: String,
    page: String,
    assets: Vec<Asset>,
}

#[derive(Serialize)]
struct Asset {
    name: String,
    url: String,
    size: u64,
}

fn client() -> Result<reqwest::Client> {
    // rustls with ring, as the rest of the core.
    static PROVIDER: Once = Once::new();
    PROVIDER.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
    Ok(reqwest::Client::builder()
        .user_agent(concat!("AirLink/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(Duration::from_secs(20))
        .build()?)
}

/// "0.3.1" newer than "0.3.0" (numbers compared one by one; a "-beta" part is left out).
fn newer(candidate: &str, current: &str) -> bool {
    let parts = |v: &str| -> Vec<u64> {
        v.split(['-', '+'])
            .next()
            .unwrap_or_default()
            .split('.')
            .map(|p| p.parse().unwrap_or(0))
            .collect()
    };
    let (a, b) = (parts(candidate), parts(current));
    for i in 0..a.len().max(b.len()) {
        let (x, y) = (
            a.get(i).copied().unwrap_or(0),
            b.get(i).copied().unwrap_or(0),
        );
        if x != y {
            return x > y;
        }
    }
    false
}

/// The latest published release when it is newer than this build, else None.
async fn check() -> Result<Option<Release>> {
    let response = client()?
        .get(LATEST)
        .header("Accept", "application/vnd.github+json")
        .send()
        .await
        .context("GitHub'a bağlanılamadı")?;
    if response.status() == reqwest::StatusCode::NOT_FOUND {
        return Ok(None); // no release published yet
    }
    if !response.status().is_success() {
        bail!("GitHub: HTTP {}", response.status().as_u16());
    }
    let v: Value = response.json().await?;
    let tag = v["tag_name"].as_str().unwrap_or_default().to_owned();
    let version = tag.trim_start_matches('v').to_owned();
    // (AIRLINK_UPDATE_FROM: as if this were that version, to try an update)
    let current = std::env::var("AIRLINK_UPDATE_FROM")
        .unwrap_or_else(|_| env!("CARGO_PKG_VERSION").to_owned());
    if version.is_empty()
        || v["draft"] == true
        || v["prerelease"] == true
        || !newer(&version, &current)
    {
        return Ok(None);
    }
    let assets = v["assets"]
        .as_array()
        .into_iter()
        .flatten()
        .map(|a| Asset {
            name: a["name"].as_str().unwrap_or_default().to_owned(),
            url: a["browser_download_url"]
                .as_str()
                .unwrap_or_default()
                .to_owned(),
            size: a["size"].as_u64().unwrap_or(0),
        })
        .collect();
    Ok(Some(Release {
        version,
        tag,
        notes: v["body"].as_str().unwrap_or_default().to_owned(),
        page: v["html_url"].as_str().unwrap_or_default().to_owned(),
        assets,
    }))
}

/// Downloads `url` into `dir` (its name the URL's last part), checks it against the SHA256SUMS at
/// `sums` (when given), and returns its path.
async fn download(
    url: &str,
    sums: &str,
    dir: &Path,
    progress: impl Fn(u64, u64),
) -> Result<PathBuf> {
    let name = url
        .rsplit('/')
        .next()
        .filter(|n| !n.is_empty() && !n.contains(".."))
        .ok_or_else(|| anyhow!("dosya adı yok: {url}"))?;
    tokio::fs::create_dir_all(dir)
        .await
        .with_context(|| format!("{} oluşturulamadı", dir.display()))?;
    let http = client()?;
    let response = http.get(url).send().await.context("indirilemedi")?;
    if !response.status().is_success() {
        bail!("indirilemedi: HTTP {}", response.status().as_u16());
    }
    let total = response.content_length().unwrap_or(0);
    let target = dir.join(name);
    let part = dir.join(format!("{name}.part"));
    let mut file = tokio::fs::File::create(&part).await?;
    let mut hash = Sha256::new();
    let mut done = 0u64;
    let mut stream = response.bytes_stream();
    progress(0, total);
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        hash.update(&chunk);
        file.write_all(&chunk).await?;
        done += chunk.len() as u64;
        progress(done, total.max(done));
    }
    file.flush().await?;
    drop(file);
    let digest: String = hash.finalize().iter().map(|b| format!("{b:02x}")).collect();
    if !sums.is_empty() {
        let list = http
            .get(sums)
            .send()
            .await?
            .error_for_status()?
            .text()
            .await?;
        let expected = list
            .lines()
            .filter_map(|l| l.split_once(char::is_whitespace))
            .find(|(_, file)| file.trim().trim_start_matches('*') == name)
            .map(|(sum, _)| sum.to_ascii_lowercase())
            .ok_or_else(|| anyhow!("SHA256SUMS'ta {name} yok"))?;
        if expected != digest {
            let _ = tokio::fs::remove_file(&part).await;
            bail!("{name} bozuk indi (SHA-256 tutmuyor)");
        }
    }
    tokio::fs::rename(&part, &target).await?;
    Ok(target)
}

/// # Safety
/// `node` as for `node_ref`.
/// Looks for a newer release. Value: {"version","tag","notes","page","assets":[{"name","url","size"}]}, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_update_check(node: *mut AirlinkNode) -> u64 {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let shared = n.shared.clone();
        n.rt.spawn(async move { shared.op_result(op, check().await) });
        op
    })
}

/// # Safety
/// `node` as for `node_ref`; strings valid C strings.
/// Downloads a release file into `dir`, checked against `sums` (the release's SHA256SUMS URL, or
/// ""): update_progress {"op","done","total"} events; value: the file's path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_update_download(
    node: *mut AirlinkNode,
    url: *const c_char,
    sums: *const c_char,
    dir: *const c_char,
) -> u64 {
    let (Some(n), Some(url), Some(sums), Some(dir)) = (
        unsafe { node_ref(node) },
        unsafe { read_str(url) },
        unsafe { read_str(sums) },
        unsafe { read_str(dir) },
    ) else {
        return 0;
    };
    guard(0, || {
        let op = n.shared.id();
        let shared: Arc<Shared> = n.shared.clone();
        n.rt.spawn(async move {
            // (at most ten a second, and always the last)
            let last = Mutex::new(Instant::now() - Duration::from_secs(1));
            let events = shared.clone();
            let progress = move |done: u64, total: u64| {
                let mut last = last.lock().unwrap();
                if done < total && last.elapsed() < Duration::from_millis(100) {
                    return;
                }
                *last = Instant::now();
                events.push(
                    json!({"event": "update_progress", "op": op, "done": done, "total": total}),
                );
            };
            let res = download(&url, &sums, Path::new(&dir), progress).await;
            shared.op_result(op, res.map(|p| p.to_string_lossy().into_owned()));
        });
        op
    })
}

#[cfg(test)]
mod tests {
    use super::newer;

    #[test]
    fn versions() {
        assert!(newer("0.3.1", "0.3.0"));
        assert!(newer("0.10.0", "0.9.9"));
        assert!(newer("1.0", "0.99.1"));
        assert!(!newer("0.3.0", "0.3.0"));
        assert!(!newer("0.2.9", "0.3.0"));
        assert!(!newer("0.3.0-beta", "0.3.0"));
    }
}
