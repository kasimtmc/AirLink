//! OneDrive through Microsoft Graph (personal, work and school accounts).

use anyhow::{Result, anyhow};
use percent_encoding::{AsciiSet, NON_ALPHANUMERIC, utf8_percent_encode};
use serde_json::{Value, json};

use crate::http::{Source, read_chunk};
use crate::{Account, Call, Entry, Provider, Quota};

const API: &str = "https://graph.microsoft.com/v1.0";
const SELECT: &str = "id,name,size,folder,file,lastModifiedDateTime";
/// Upload-session chunks must be multiples of 320 KiB: 32 of them.
const CHUNK: usize = 32 * 320 * 1024;
/// Smaller files go in one request.
const SIMPLE_MAX: u64 = 4 * 1024 * 1024;

const PATH: &AsciiSet = &NON_ALPHANUMERIC.remove(b'-').remove(b'_').remove(b'.').remove(b'~');

fn item(id: &str) -> String {
    if id.is_empty() { format!("{API}/me/drive/root") } else { format!("{API}/me/drive/items/{}", utf8_percent_encode(id, PATH)) }
}

fn entry(v: &Value) -> Entry {
    Entry {
        id: v["id"].as_str().unwrap_or_default().to_owned(),
        name: v["name"].as_str().unwrap_or_default().to_owned(),
        folder: v.get("folder").is_some_and(|f| !f.is_null()),
        size: v["size"].as_u64().unwrap_or(0),
        modified: v["lastModifiedDateTime"].as_str().unwrap_or_default().to_owned(),
        kind: String::new(),
    }
}

pub(crate) async fn account(http: &reqwest::Client, access: &str) -> Result<Account> {
    let me: Value = crate::http::check(http.get(format!("{API}/me?$select=id,displayName,userPrincipalName,mail")).bearer_auth(access).send().await?)
        .await?
        .json()
        .await?;
    let id = me["id"].as_str().ok_or_else(|| anyhow!("Microsoft hesabı okunamadı"))?;
    let email = me["mail"].as_str().or(me["userPrincipalName"].as_str()).unwrap_or_default();
    Ok(Account {
        id: format!("onedrive:{id}"),
        provider: Provider::Onedrive,
        name: me["displayName"].as_str().filter(|n| !n.is_empty()).unwrap_or(email).to_owned(),
        email: email.to_owned(),
    })
}

/// Follows `@odata.nextLink` pages.
async fn pages(call: &Call<'_>, first: String) -> Result<Vec<Entry>> {
    let mut url = Some(first);
    let mut out = Vec::new();
    while let Some(u) = url.take() {
        let page = call.json(|h, t| h.get(&u).bearer_auth(t)).await?;
        out.extend(page["value"].as_array().into_iter().flatten().map(entry));
        url = page["@odata.nextLink"].as_str().map(str::to_owned);
    }
    Ok(out)
}

pub(crate) async fn list(call: &Call<'_>, folder: &str) -> Result<Vec<Entry>> {
    pages(call, format!("{}/children?$select={SELECT}&$top=999", item(folder))).await
}

pub(crate) async fn search(call: &Call<'_>, query: &str) -> Result<Vec<Entry>> {
    let q = query.replace('\'', "''");
    pages(call, format!("{API}/me/drive/root/search(q='{}')?$select={SELECT}&$top=200", utf8_percent_encode(&q, PATH))).await
}

pub(crate) async fn quota(call: &Call<'_>) -> Result<Quota> {
    let v = call.json(|h, t| h.get(format!("{API}/me/drive?$select=quota")).bearer_auth(t)).await?;
    Ok(Quota { used: v["quota"]["used"].as_u64().unwrap_or(0), total: v["quota"]["total"].as_u64().unwrap_or(0) })
}

pub(crate) async fn download(call: &Call<'_>, id: &str) -> Result<reqwest::Response> {
    // (Graph answers with a redirect to a pre-signed address; the token is not sent there)
    call.send(|h, t| h.get(format!("{}/content", item(id))).bearer_auth(t)).await
}

pub(crate) async fn upload(call: &Call<'_>, folder: &str, name: &str, source: Source) -> Result<Entry> {
    let target = format!("{}:/{}:", item(folder), utf8_percent_encode(name, PATH));
    let progress = source.progress.clone();
    let total = source.size;
    progress(0, total);
    if total <= SIMPLE_MAX {
        let body = tokio::fs::read(&source.path).await?;
        let v = call
            .json(|h, t| {
                h.put(format!("{target}/content?@microsoft.graph.conflictBehavior=rename"))
                    .bearer_auth(t)
                    .header("Content-Type", "application/octet-stream")
                    .body(body.clone())
            })
            .await?;
        progress(total, total);
        return Ok(entry(&v));
    }
    let session = call
        .json(|h, t| {
            h.post(format!("{target}/createUploadSession"))
                .bearer_auth(t)
                .json(&json!({"item": {"@microsoft.graph.conflictBehavior": "rename"}}))
        })
        .await?;
    let upload = session["uploadUrl"].as_str().ok_or_else(|| anyhow!("OneDrive yükleme oturumu açılamadı"))?.to_owned();
    let mut file = source.open().await?;
    let mut offset = 0u64;
    loop {
        let chunk = read_chunk(&mut file, CHUNK).await?;
        if chunk.is_empty() {
            return Err(anyhow!("dosya yüklenirken kısaldı"));
        }
        let end = offset + chunk.len() as u64;
        // The session's address is pre-authorized: no token.
        let response = call
            .http()
            .put(&upload)
            .header("Content-Range", format!("bytes {offset}-{}/{total}", end - 1))
            .body(chunk)
            .send()
            .await?;
        let response = crate::http::check(response).await?;
        offset = end;
        progress(offset, total);
        if offset >= total {
            return Ok(entry(&response.json().await?));
        }
    }
}

pub(crate) async fn create_folder(call: &Call<'_>, parent: &str, name: &str) -> Result<Entry> {
    let v = call
        .json(|h, t| {
            h.post(format!("{}/children", item(parent)))
                .bearer_auth(t)
                .json(&json!({"name": name, "folder": {}, "@microsoft.graph.conflictBehavior": "rename"}))
        })
        .await?;
    Ok(entry(&v))
}

pub(crate) async fn rename(call: &Call<'_>, id: &str, name: &str) -> Result<Entry> {
    let v = call.json(|h, t| h.patch(item(id)).bearer_auth(t).json(&json!({"name": name}))).await?;
    Ok(entry(&v))
}

pub(crate) async fn delete(call: &Call<'_>, id: &str) -> Result<()> {
    call.send(|h, t| h.delete(item(id)).bearer_auth(t)).await?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn items() {
        assert_eq!(item(""), "https://graph.microsoft.com/v1.0/me/drive/root");
        assert_eq!(item("A1!2"), "https://graph.microsoft.com/v1.0/me/drive/items/A1%212");
        let e = entry(&json!({"id": "1", "name": "a", "folder": {"childCount": 2}, "lastModifiedDateTime": "2026-01-01T00:00:00Z"}));
        assert!(e.folder && e.size == 0);
        assert!(!entry(&json!({"id": "2", "name": "b", "file": {}, "size": 5})).folder);
    }
}
