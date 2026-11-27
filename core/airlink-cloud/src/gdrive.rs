//! Google Drive (API v3). Google's own documents have no file to download: they are exported as
//! Office files (drawings as PNG).

use anyhow::{Result, anyhow, bail};
use serde_json::{Value, json};

use crate::http::{Source, read_chunk};
use crate::{Account, Call, Entry, Provider, Quota};

const API: &str = "https://www.googleapis.com/drive/v3";
const UPLOAD: &str = "https://www.googleapis.com/upload/drive/v3/files";
const FIELDS: &str = "id,name,mimeType,size,modifiedTime,thumbnailLink";
const FOLDER: &str = "application/vnd.google-apps.folder";
/// Resumable-upload chunks must be multiples of 256 KiB: 32 of them.
const CHUNK: usize = 32 * 256 * 1024;

fn entry(v: &Value) -> Entry {
    let mime = v["mimeType"].as_str().unwrap_or_default();
    Entry {
        id: v["id"].as_str().unwrap_or_default().to_owned(),
        name: v["name"].as_str().unwrap_or_default().to_owned(),
        folder: mime == FOLDER,
        // (sizes come as strings)
        size: v["size"].as_str().and_then(|s| s.parse().ok()).unwrap_or(0),
        modified: v["modifiedTime"].as_str().unwrap_or_default().to_owned(),
        kind: if mime.starts_with("application/vnd.google-apps.") && mime != FOLDER {
            mime.to_owned()
        } else {
            String::new()
        },
        thumb: v["thumbnailLink"].as_str().unwrap_or_default().to_owned(),
    }
}

/// Google's document types as Office files: (MIME type to export to, file extension).
fn export(kind: &str) -> Option<(&'static str, &'static str)> {
    Some(match kind.strip_prefix("application/vnd.google-apps.")? {
        "document" => (
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
            "docx",
        ),
        "spreadsheet" => (
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
            "xlsx",
        ),
        "presentation" => (
            "application/vnd.openxmlformats-officedocument.presentationml.presentation",
            "pptx",
        ),
        "drawing" => ("image/png", "png"),
        _ => return None,
    })
}

fn quote(s: &str) -> String {
    s.replace('\\', "\\\\").replace('\'', "\\'")
}

fn folder_id(id: &str) -> &str {
    if id.is_empty() { "root" } else { id }
}

pub(crate) async fn account(http: &reqwest::Client, access: &str) -> Result<Account> {
    let about: Value = crate::http::check(
        http.get(format!("{API}/about"))
            .query(&[("fields", "user(displayName,emailAddress,permissionId)")])
            .bearer_auth(access)
            .send()
            .await?,
    )
    .await?
    .json()
    .await?;
    let user = &about["user"];
    let id = user["permissionId"]
        .as_str()
        .ok_or_else(|| anyhow!("Google hesabı okunamadı"))?;
    let email = user["emailAddress"].as_str().unwrap_or_default();
    Ok(Account {
        id: format!("gdrive:{id}"),
        provider: Provider::Gdrive,
        name: user["displayName"]
            .as_str()
            .filter(|n| !n.is_empty())
            .unwrap_or(email)
            .to_owned(),
        email: email.to_owned(),
    })
}

async fn files(call: &Call<'_>, q: String) -> Result<Vec<Entry>> {
    let mut out = Vec::new();
    let mut token: Option<String> = None;
    loop {
        let fields = format!("nextPageToken,files({FIELDS})");
        let mut query = vec![
            ("q", q.clone()),
            ("fields", fields),
            ("pageSize", "1000".to_owned()),
            ("orderBy", "folder,name".to_owned()),
        ];
        if let Some(t) = &token {
            query.push(("pageToken", t.clone()));
        }
        let page = call
            .json(|h, t| h.get(format!("{API}/files")).query(&query).bearer_auth(t))
            .await?;
        out.extend(page["files"].as_array().into_iter().flatten().map(entry));
        token = page["nextPageToken"].as_str().map(str::to_owned);
        if token.is_none() {
            return Ok(out);
        }
    }
}

pub(crate) async fn list(call: &Call<'_>, folder: &str) -> Result<Vec<Entry>> {
    files(
        call,
        format!(
            "'{}' in parents and trashed = false",
            quote(folder_id(folder))
        ),
    )
    .await
}

pub(crate) async fn search(call: &Call<'_>, query: &str) -> Result<Vec<Entry>> {
    files(
        call,
        format!("name contains '{}' and trashed = false", quote(query)),
    )
    .await
}

pub(crate) async fn quota(call: &Call<'_>) -> Result<Quota> {
    let v = call
        .json(|h, t| {
            h.get(format!("{API}/about"))
                .query(&[("fields", "storageQuota")])
                .bearer_auth(t)
        })
        .await?;
    let n = |k: &str| {
        v["storageQuota"][k]
            .as_str()
            .and_then(|s| s.parse().ok())
            .unwrap_or(0)
    };
    Ok(Quota {
        used: n("usage"),
        total: n("limit"),
    })
}

/// The picture from the listing's thumbnailLink, asked for at 256 pixels (its "=s220" ending).
pub(crate) async fn thumbnail(call: &Call<'_>, entry: &Entry) -> Result<Option<Vec<u8>>> {
    if entry.thumb.is_empty() {
        return Ok(None);
    }
    let url = match entry.thumb.rsplit_once("=s") {
        Some((base, size)) if size.chars().all(|c| c.is_ascii_digit()) => format!("{base}=s256"),
        _ => entry.thumb.clone(),
    };
    call.picture(|h, t| h.get(&url).bearer_auth(t)).await
}

/// The file's content and the name to save it as.
pub(crate) async fn download(
    call: &Call<'_>,
    entry: &Entry,
) -> Result<(reqwest::Response, String)> {
    if entry.kind.is_empty() {
        let url = format!("{API}/files/{}", entry.id);
        let response = call
            .send(|h, t| h.get(&url).query(&[("alt", "media")]).bearer_auth(t))
            .await?;
        return Ok((response, entry.name.clone()));
    }
    let Some((mime, ext)) = export(&entry.kind) else {
        bail!(
            "bu Google dosyası indirilemiyor (yalnızca Dokümanlar, E-Tablolar, Slaytlar ve Çizimler)"
        );
    };
    let url = format!("{API}/files/{}/export", entry.id);
    let response = call
        .send(|h, t| h.get(&url).query(&[("mimeType", mime)]).bearer_auth(t))
        .await?;
    let name = if entry.name.to_lowercase().ends_with(&format!(".{ext}")) {
        entry.name.clone()
    } else {
        format!("{}.{ext}", entry.name)
    };
    Ok((response, name))
}

pub(crate) async fn upload(
    call: &Call<'_>,
    folder: &str,
    name: &str,
    source: Source,
) -> Result<Entry> {
    let progress = source.progress.clone();
    let total = source.size;
    progress(0, total);
    let meta = json!({"name": name, "parents": [folder_id(folder)]});
    let start = call
        .send(|h, t| {
            h.post(UPLOAD)
                .query(&[("uploadType", "resumable"), ("fields", FIELDS)])
                .bearer_auth(t)
                .header("X-Upload-Content-Length", total.to_string())
                .json(&meta)
        })
        .await?;
    let session = start
        .headers()
        .get(reqwest::header::LOCATION)
        .and_then(|v| v.to_str().ok())
        .ok_or_else(|| anyhow!("Google Drive yükleme oturumu açılamadı"))?
        .to_owned();
    let mut file = source.open().await?;
    let mut offset = 0u64;
    loop {
        let chunk = read_chunk(&mut file, CHUNK).await?;
        let end = offset + chunk.len() as u64;
        let mut request = call.http().put(&session);
        if total > 0 {
            if chunk.is_empty() {
                bail!("dosya yüklenirken kısaldı");
            }
            request = request.header(
                "Content-Range",
                format!("bytes {offset}-{}/{total}", end - 1),
            );
        }
        let response = request.body(chunk).send().await?;
        // 308: the chunk is in, more to come.
        if response.status().as_u16() == 308 {
            offset = end;
            progress(offset, total);
            continue;
        }
        let response = crate::http::check(response).await?;
        progress(total, total);
        return Ok(entry(&response.json().await?));
    }
}

pub(crate) async fn create_folder(call: &Call<'_>, parent: &str, name: &str) -> Result<Entry> {
    let body = json!({"name": name, "mimeType": FOLDER, "parents": [folder_id(parent)]});
    let v = call
        .json(|h, t| {
            h.post(format!("{API}/files"))
                .query(&[("fields", FIELDS)])
                .bearer_auth(t)
                .json(&body)
        })
        .await?;
    Ok(entry(&v))
}

pub(crate) async fn rename(call: &Call<'_>, id: &str, name: &str) -> Result<Entry> {
    let url = format!("{API}/files/{id}");
    let v = call
        .json(|h, t| {
            h.patch(&url)
                .query(&[("fields", FIELDS)])
                .bearer_auth(t)
                .json(&json!({"name": name}))
        })
        .await?;
    Ok(entry(&v))
}

pub(crate) async fn delete(call: &Call<'_>, id: &str) -> Result<()> {
    let url = format!("{API}/files/{id}");
    call.send(|h, t| h.patch(&url).bearer_auth(t).json(&json!({"trashed": true})))
        .await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn entries() {
        let doc = entry(
            &json!({"id": "1", "name": "Notlar", "mimeType": "application/vnd.google-apps.document"}),
        );
        assert_eq!(doc.kind, "application/vnd.google-apps.document");
        assert_eq!(export(&doc.kind).unwrap().1, "docx");
        let dir = entry(&json!({"id": "2", "name": "D", "mimeType": FOLDER}));
        assert!(dir.folder && dir.kind.is_empty());
        assert_eq!(
            entry(&json!({"id": "3", "name": "f", "mimeType": "text/plain", "size": "12"})).size,
            12
        );
        assert_eq!(quote("it's"), "it\\'s");
    }
}
