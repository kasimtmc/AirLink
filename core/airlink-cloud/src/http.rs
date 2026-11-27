//! The HTTPS client, error messages from the providers' replies, and file reading and writing
//! for transfers.

use std::path::{Path, PathBuf};
use std::sync::Once;

use anyhow::{Context, Result, anyhow};
use futures_util::StreamExt;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

use crate::Progress;

pub(crate) fn client() -> Result<reqwest::Client> {
    // rustls with ring, the same crypto as the core's QUIC (aws-lc does not cross-build here).
    static PROVIDER: Once = Once::new();
    PROVIDER.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
    Ok(reqwest::Client::builder()
        .user_agent(concat!("AirLink/", env!("CARGO_PKG_VERSION")))
        .connect_timeout(std::time::Duration::from_secs(20))
        .build()?)
}

/// The response if its status is a success, else an error with the provider's own message.
pub(crate) async fn check(response: reqwest::Response) -> Result<reqwest::Response> {
    let status = response.status();
    if status.is_success() {
        return Ok(response);
    }
    let body = response.text().await.unwrap_or_default();
    Err(anyhow!("{}", error_message(status, &body)))
}

pub(crate) fn error_message(status: reqwest::StatusCode, body: &str) -> String {
    let message = serde_json::from_str::<serde_json::Value>(body).ok().and_then(|v| {
        [
            v["error"]["message"].as_str(),     // Microsoft Graph, Google
            v["error_description"].as_str(),    // OAuth
            v["error_summary"].as_str(),        // Dropbox
            v["error"].as_str(),
        ]
        .into_iter()
        .flatten()
        .find(|m| !m.is_empty())
        .map(str::to_owned)
    });
    let text = message.unwrap_or_else(|| body.chars().take(200).collect());
    match status.as_u16() {
        401 => format!("oturum geçersiz, hesabı yeniden bağlayın ({text})"),
        403 => format!("izin yok ({text})"),
        404 => format!("bulunamadı ({text})"),
        507 => "sürücüde yer kalmadı".to_owned(),
        _ => format!("HTTP {}: {text}", status.as_u16()),
    }
}

/// A file to upload.
pub(crate) struct Source {
    pub path: PathBuf,
    pub size: u64,
    pub progress: Progress,
}

impl Source {
    pub async fn open(&self) -> Result<tokio::fs::File> {
        tokio::fs::File::open(&self.path).await.with_context(|| format!("{} açılamadı", self.path.display()))
    }
}

/// Reads up to `max` bytes (less only at the end of the file).
pub(crate) async fn read_chunk(file: &mut tokio::fs::File, max: usize) -> Result<Vec<u8>> {
    let mut chunk = vec![0; max];
    let mut filled = 0;
    while filled < max {
        let n = file.read(&mut chunk[filled..]).await?;
        if n == 0 {
            break;
        }
        filled += n;
    }
    chunk.truncate(filled);
    Ok(chunk)
}

/// Writes a download into `dir` as `name` (or `name (2)` and on, when taken), through a
/// `.part` file that is renamed when it is complete.
pub(crate) async fn save(response: reqwest::Response, dir: &Path, name: &str, size: u64, progress: Progress) -> Result<PathBuf> {
    tokio::fs::create_dir_all(dir).await.with_context(|| format!("{} oluşturulamadı", dir.display()))?;
    let total = response.content_length().unwrap_or(size);
    let target = free_name(dir, &safe_name(name));
    let mut part_name = target.file_name().unwrap_or_default().to_os_string();
    part_name.push(".part");
    let part = target.with_file_name(part_name);
    let result = async {
        let mut file = tokio::fs::File::create(&part).await.with_context(|| format!("{} yazılamadı", part.display()))?;
        let mut stream = response.bytes_stream();
        let mut done = 0u64;
        progress(0, total);
        while let Some(chunk) = stream.next().await {
            let chunk = chunk?;
            file.write_all(&chunk).await?;
            done += chunk.len() as u64;
            progress(done, total.max(done));
        }
        file.flush().await?;
        drop(file);
        tokio::fs::rename(&part, &target).await?;
        anyhow::Ok(())
    }
    .await;
    if result.is_err() {
        let _ = tokio::fs::remove_file(&part).await;
    }
    result.map(|()| target)
}

/// A name that is safe as a file name on Linux and Windows.
fn safe_name(name: &str) -> String {
    let cleaned: String = name
        .chars()
        .map(|c| if matches!(c, '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|') || c.is_control() { '_' } else { c })
        .collect();
    let trimmed = cleaned.trim().trim_end_matches('.');
    if trimmed.is_empty() { "dosya".to_owned() } else { trimmed.to_owned() }
}

fn free_name(dir: &Path, name: &str) -> PathBuf {
    let first = dir.join(name);
    if !first.exists() {
        return first;
    }
    let (stem, ext) = match name.rsplit_once('.') {
        Some((s, e)) if !s.is_empty() => (s.to_owned(), format!(".{e}")),
        _ => (name.to_owned(), String::new()),
    };
    (2..)
        .map(|n| dir.join(format!("{stem} ({n}){ext}")))
        .find(|p| !p.exists())
        .unwrap_or(first)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        assert_eq!(safe_name("a/b:c?.txt"), "a_b_c_.txt");
        assert_eq!(safe_name(" . "), "dosya");
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("x.txt"), "").unwrap();
        std::fs::write(dir.path().join("x (2).txt"), "").unwrap();
        assert_eq!(free_name(dir.path(), "x.txt"), dir.path().join("x (3).txt"));
        assert_eq!(free_name(dir.path(), "y"), dir.path().join("y"));
    }

    #[test]
    fn messages() {
        let s = reqwest::StatusCode::BAD_REQUEST;
        assert_eq!(error_message(s, r#"{"error":{"message":"bad"}}"#), "HTTP 400: bad");
        assert_eq!(error_message(s, r#"{"error_summary":"path/not_found/"}"#), "HTTP 400: path/not_found/");
        assert_eq!(error_message(s, "plain"), "HTTP 400: plain");
    }
}
