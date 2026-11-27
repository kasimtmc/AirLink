//! Dropbox (API v2). Entries are named by their path (`/Folder/file.txt`; the root is `""`).

use anyhow::{Result, anyhow};
use serde_json::{Value, json};

use crate::http::{Source, read_chunk};
use crate::{Account, Call, Entry, Provider, Quota};

const API: &str = "https://api.dropboxapi.com/2";
const CONTENT: &str = "https://content.dropboxapi.com/2";
/// Upload-session chunks (the API takes up to 150 MB each).
const CHUNK: usize = 8 * 1024 * 1024;

/// The `Dropbox-API-Arg` header: JSON with every non-ASCII character escaped, as HTTP headers
/// must be ASCII.
fn arg(v: &Value) -> String {
    let mut out = String::new();
    for c in v.to_string().chars() {
        if c.is_ascii() {
            out.push(c);
        } else {
            let mut units = [0u16; 2];
            for u in c.encode_utf16(&mut units) {
                out.push_str(&format!("\\u{u:04x}"));
            }
        }
    }
    out
}

fn entry(v: &Value) -> Entry {
    Entry {
        id: v["path_display"].as_str().unwrap_or_default().to_owned(),
        name: v["name"].as_str().unwrap_or_default().to_owned(),
        folder: v[".tag"] == "folder",
        size: v["size"].as_u64().unwrap_or(0),
        modified: v["server_modified"].as_str().unwrap_or_default().to_owned(),
        kind: String::new(),
        thumb: String::new(),
    }
}

fn child(parent: &str, name: &str) -> String {
    format!("{}/{name}", parent.trim_end_matches('/'))
}

pub(crate) async fn account(http: &reqwest::Client, access: &str) -> Result<Account> {
    let me: Value = crate::http::check(
        http.post(format!("{API}/users/get_current_account"))
            .bearer_auth(access)
            .header("Content-Type", "application/json")
            .body("null")
            .send()
            .await?,
    )
    .await?
    .json()
    .await?;
    let id = me["account_id"]
        .as_str()
        .ok_or_else(|| anyhow!("Dropbox hesabı okunamadı"))?;
    let email = me["email"].as_str().unwrap_or_default();
    Ok(Account {
        id: format!("dropbox:{id}"),
        provider: Provider::Dropbox,
        name: me["name"]["display_name"]
            .as_str()
            .filter(|n| !n.is_empty())
            .unwrap_or(email)
            .to_owned(),
        email: email.to_owned(),
    })
}

async fn rpc(call: &Call<'_>, endpoint: &str, body: Value) -> Result<Value> {
    call.json(|h, t| {
        h.post(format!("{API}/{endpoint}"))
            .bearer_auth(t)
            .json(&body)
    })
    .await
}

pub(crate) async fn list(call: &Call<'_>, folder: &str) -> Result<Vec<Entry>> {
    let mut page = rpc(
        call,
        "files/list_folder",
        json!({"path": folder, "limit": 2000}),
    )
    .await?;
    let mut out = Vec::new();
    loop {
        out.extend(
            page["entries"]
                .as_array()
                .into_iter()
                .flatten()
                .filter(|e| e[".tag"] != "deleted")
                .map(entry),
        );
        if page["has_more"] != true {
            return Ok(out);
        }
        let cursor = page["cursor"].clone();
        page = rpc(
            call,
            "files/list_folder/continue",
            json!({"cursor": cursor}),
        )
        .await?;
    }
}

pub(crate) async fn search(call: &Call<'_>, query: &str) -> Result<Vec<Entry>> {
    let v = rpc(
        call,
        "files/search_v2",
        json!({"query": query, "options": {"max_results": 200}}),
    )
    .await?;
    Ok(v["matches"]
        .as_array()
        .into_iter()
        .flatten()
        .map(|m| entry(&m["metadata"]["metadata"]))
        .filter(|e| !e.id.is_empty())
        .collect())
}

pub(crate) async fn quota(call: &Call<'_>) -> Result<Quota> {
    let v = call
        .json(|h, t| {
            h.post(format!("{API}/users/get_space_usage"))
                .bearer_auth(t)
                .header("Content-Type", "application/json")
                .body("null")
        })
        .await?;
    Ok(Quota {
        used: v["used"].as_u64().unwrap_or(0),
        total: v["allocation"]["allocated"].as_u64().unwrap_or(0),
    })
}

pub(crate) async fn download(call: &Call<'_>, path: &str) -> Result<reqwest::Response> {
    let header = arg(&json!({"path": path}));
    call.send(|h, t| {
        h.post(format!("{CONTENT}/files/download"))
            .bearer_auth(t)
            .header("Dropbox-API-Arg", &header)
    })
    .await
}

/// Dropbox makes pictures of images only (not videos).
pub(crate) async fn thumbnail(call: &Call<'_>, entry: &Entry) -> Result<Option<Vec<u8>>> {
    let ext = entry
        .name
        .rsplit_once('.')
        .map(|(_, e)| e.to_ascii_lowercase())
        .unwrap_or_default();
    if !matches!(
        ext.as_str(),
        "jpg" | "jpeg" | "png" | "tiff" | "tif" | "gif" | "webp" | "ppm" | "bmp" | "heic"
    ) {
        return Ok(None);
    }
    let header = arg(
        &json!({"resource": {".tag": "path", "path": entry.id}, "format": "jpeg", "size": "w256h256", "mode": "fitone_bestfit"}),
    );
    call.picture(|h, t| {
        h.post(format!("{CONTENT}/files/get_thumbnail_v2"))
            .bearer_auth(t)
            .header("Dropbox-API-Arg", &header)
    })
    .await
}

pub(crate) async fn upload(
    call: &Call<'_>,
    folder: &str,
    name: &str,
    source: Source,
) -> Result<Entry> {
    let commit = json!({"path": child(folder, name), "mode": "add", "autorename": true});
    let progress = source.progress.clone();
    let total = source.size;
    progress(0, total);
    let mut file = source.open().await?;
    let content = |endpoint: &str, header: Value, chunk: Vec<u8>| {
        let (url, header) = (format!("{CONTENT}/{endpoint}"), arg(&header));
        async move {
            call.send(|h, t| {
                h.post(&url)
                    .bearer_auth(t)
                    .header("Dropbox-API-Arg", &header)
                    .header("Content-Type", "application/octet-stream")
                    .body(chunk.clone())
            })
            .await
        }
    };
    if total <= CHUNK as u64 {
        let chunk = read_chunk(&mut file, CHUNK).await?;
        let v: Value = content("files/upload", commit, chunk).await?.json().await?;
        progress(total, total);
        return Ok(entry(&v));
    }
    let first = read_chunk(&mut file, CHUNK).await?;
    let mut offset = first.len() as u64;
    let start: Value = content("files/upload_session/start", json!({"close": false}), first)
        .await?
        .json()
        .await?;
    let session = start["session_id"]
        .as_str()
        .ok_or_else(|| anyhow!("Dropbox yükleme oturumu açılamadı"))?
        .to_owned();
    progress(offset, total);
    loop {
        let chunk = read_chunk(&mut file, CHUNK).await?;
        let len = chunk.len() as u64;
        let cursor = json!({"session_id": session, "offset": offset});
        if offset + len >= total {
            let v: Value = content(
                "files/upload_session/finish",
                json!({"cursor": cursor, "commit": commit}),
                chunk,
            )
            .await?
            .json()
            .await?;
            progress(total, total);
            return Ok(entry(&v));
        }
        if chunk.is_empty() {
            return Err(anyhow!("dosya yüklenirken kısaldı"));
        }
        content(
            "files/upload_session/append_v2",
            json!({"cursor": cursor, "close": false}),
            chunk,
        )
        .await?;
        offset += len;
        progress(offset, total);
    }
}

pub(crate) async fn create_folder(call: &Call<'_>, parent: &str, name: &str) -> Result<Entry> {
    let v = rpc(
        call,
        "files/create_folder_v2",
        json!({"path": child(parent, name), "autorename": true}),
    )
    .await?;
    let mut e = entry(&v["metadata"]);
    e.folder = true;
    Ok(e)
}

pub(crate) async fn rename(call: &Call<'_>, path: &str, name: &str) -> Result<Entry> {
    let parent = path.rsplit_once('/').map(|(p, _)| p).unwrap_or("");
    let v = rpc(
        call,
        "files/move_v2",
        json!({"from_path": path, "to_path": child(parent, name), "autorename": false}),
    )
    .await?;
    Ok(entry(&v["metadata"]))
}

pub(crate) async fn delete(call: &Call<'_>, path: &str) -> Result<()> {
    rpc(call, "files/delete_v2", json!({"path": path})).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_is_ascii() {
        assert_eq!(
            arg(&json!({"path": "/A\u{11f}a\u{e7} \u{1f600}"})),
            "{\"path\":\"/A\\u011fa\\u00e7 \\ud83d\\ude00\"}"
        );
    }

    #[test]
    fn paths() {
        assert_eq!(child("", "a.txt"), "/a.txt");
        assert_eq!(child("/Docs", "a.txt"), "/Docs/a.txt");
        let e = entry(&json!({".tag": "folder", "name": "Docs", "path_display": "/Docs"}));
        assert!(e.folder && e.id == "/Docs");
    }
}
