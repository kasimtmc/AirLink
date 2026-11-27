//! Cloud drives (airlink-cloud) for the app: see `airlink_cloud_*` in airlink.h. Each call but
//! `airlink_cloud_state` and `airlink_cloud_cancel` is an operation whose result is an op_result
//! event; transfers also send `cloud_progress` and sign-in `cloud_auth` with the page to open.
//! Android has none of it: there the providers' own apps reach their drives.
#![cfg_attr(target_os = "android", allow(unused))]

use std::collections::HashMap;
use std::ffi::c_char;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::json;

use crate::{AirlinkNode, Shared, guard, node_ref, read_str, to_c};

#[cfg(not(target_os = "android"))]
pub(crate) type Cloud = airlink_cloud::Cloud;
#[cfg(target_os = "android")]
pub(crate) type Cloud = ();

/// The node's cloud drives and their running operations (to cancel).
#[derive(Default)]
pub(crate) struct Drives {
    pub cloud: Option<Arc<Cloud>>,
    pub running: Arc<Mutex<HashMap<u64, tokio::task::AbortHandle>>>,
}

#[cfg(not(target_os = "android"))]
pub(crate) fn open(data_dir: &std::path::Path) -> Option<Arc<Cloud>> {
    airlink_cloud::Cloud::new(data_dir)
        .map_err(|e| tracing::warn!("cloud drives unavailable: {e:#}"))
        .ok()
        .map(Arc::new)
}

#[cfg(target_os = "android")]
pub(crate) fn open(_: &std::path::Path) -> Option<Arc<Cloud>> {
    None
}

/// Progress as `cloud_progress` events, at most ten a second (and always the last).
#[cfg(not(target_os = "android"))]
fn progress(shared: Arc<Shared>, op: u64) -> airlink_cloud::Progress {
    let last = Mutex::new(Instant::now() - Duration::from_secs(1));
    Arc::new(move |done, total| {
        let mut last = last.lock().unwrap();
        if done < total && last.elapsed() < Duration::from_millis(100) {
            return;
        }
        *last = Instant::now();
        shared.push(json!({"event": "cloud_progress", "op": op, "done": done, "total": total}));
    })
}

/// Runs `f` on the node's runtime as operation `op`, reporting its result.
#[cfg(not(target_os = "android"))]
fn spawn<T, F>(n: &AirlinkNode, f: impl FnOnce(Arc<Cloud>, Arc<Shared>, u64) -> F) -> u64
where
    T: serde::Serialize,
    F: Future<Output = anyhow::Result<T>> + Send + 'static,
{
    let Some(cloud) = n.drives.cloud.clone() else {
        return 0;
    };
    let op = n.shared.id();
    let (shared, running) = (n.shared.clone(), n.drives.running.clone());
    let work = f(cloud, shared.clone(), op);
    // (held while the task is added, so a quick task cannot finish before it is listed)
    let mut list = running.lock().unwrap();
    let ops = running.clone();
    let task = n.rt.spawn(async move {
        let res = work.await;
        ops.lock().unwrap().remove(&op);
        shared.op_result(op, res);
    });
    list.insert(op, task.abort_handle());
    op
}

/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_cloud_state(node: *mut AirlinkNode) -> *mut c_char {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return std::ptr::null_mut();
    };
    guard(std::ptr::null_mut(), || {
        #[cfg(not(target_os = "android"))]
        let state = match &n.drives.cloud {
            Some(c) => json!({"providers": c.providers(), "accounts": c.accounts()}),
            None => json!({"providers": [], "accounts": []}),
        };
        #[cfg(target_os = "android")]
        let state = {
            let _ = n;
            json!({"providers": [], "accounts": []})
        };
        to_c(state.to_string())
    })
}

/// Stops a running cloud operation; its op_result says "cancelled". False if it is not running.
///
/// # Safety
/// `node` as for `node_ref`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn airlink_cloud_cancel(node: *mut AirlinkNode, op: u64) -> bool {
    let Some(n) = (unsafe { node_ref(node) }) else {
        return false;
    };
    guard(false, || {
        let Some(task) = n.drives.running.lock().unwrap().remove(&op) else {
            return false;
        };
        task.abort();
        n.shared.op_result::<()>(op, Err(anyhow::anyhow!("cancelled")));
        true
    })
}

macro_rules! cloud_op {
    ($(#[$doc:meta])* fn $name:ident($($arg:ident),*) |$cloud:ident, $shared:ident, $op:ident| $body:block) => {
        $(#[$doc])*
        /// # Safety
        /// `node` as for `node_ref`; strings valid C strings.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn $name(node: *mut AirlinkNode, $($arg: *const c_char),*) -> u64 {
            let Some(n) = (unsafe { node_ref(node) }) else { return 0 };
            $(let Some($arg) = (unsafe { read_str($arg) }) else { return 0 };)*
            #[cfg(not(target_os = "android"))]
            {
                guard(0, || spawn(n, |$cloud, $shared, $op| {
                    let _ = (&$shared, $op);
                    async move $body
                }))
            }
            #[cfg(target_os = "android")]
            {
                let _ = (n, $($arg),*);
                0
            }
        }
    };
}

cloud_op! {
    /// Signs in to a provider ("onedrive", "dropbox", "gdrive") in the browser: a cloud_auth event
    /// brings the page to open. Value: the account.
    fn airlink_cloud_connect(provider) |cloud, shared, op| {
        let p = airlink_cloud::Provider::parse(&provider).ok_or_else(|| anyhow::anyhow!("unknown provider {provider}"))?;
        cloud.connect(p, |url| shared.push(json!({"event": "cloud_auth", "op": op, "url": url}))).await
    }
}

cloud_op! {
    /// Forgets an account and its sign-in on this computer.
    fn airlink_cloud_disconnect(account) |cloud, shared, op| {
        tokio::task::spawn_blocking(move || cloud.disconnect(&account)).await?
    }
}

cloud_op! {
    /// A folder's entries ("" is the root). Value: [entry].
    fn airlink_cloud_list(account, folder) |cloud, shared, op| { cloud.list(&account, &folder).await }
}

cloud_op! {
    /// Entries anywhere in the drive whose names match. Value: [entry].
    fn airlink_cloud_search(account, query) |cloud, shared, op| { cloud.search(&account, &query).await }
}

cloud_op! {
    /// Value: {"used","total"} in bytes (total 0: unknown).
    fn airlink_cloud_quota(account) |cloud, shared, op| { cloud.quota(&account).await }
}

cloud_op! {
    /// Downloads an entry (as JSON, from a listing) into `dir`. Value: the file's path.
    fn airlink_cloud_download(account, entry_json, dir) |cloud, shared, op| {
        let entry: airlink_cloud::Entry = serde_json::from_str(&entry_json)?;
        let path = cloud.download(&account, &entry, &PathBuf::from(dir), progress(shared.clone(), op)).await?;
        anyhow::Ok(path.to_string_lossy().into_owned())
    }
}

cloud_op! {
    /// Uploads a local file into `folder`. Value: the new entry.
    fn airlink_cloud_upload(account, folder, path) |cloud, shared, op| {
        cloud.upload(&account, &folder, &PathBuf::from(path), progress(shared.clone(), op)).await
    }
}

cloud_op! {
    /// Value: the new folder's entry.
    fn airlink_cloud_create_folder(account, parent, name) |cloud, shared, op| { cloud.create_folder(&account, &parent, &name).await }
}

cloud_op! {
    /// Value: the renamed entry (a Dropbox entry's id changes with its name).
    fn airlink_cloud_rename(account, id, name) |cloud, shared, op| { cloud.rename(&account, &id, &name).await }
}

cloud_op! {
    /// Moves an entry to the drive's recycle bin.
    fn airlink_cloud_delete(account, id) |cloud, shared, op| { cloud.delete(&account, &id).await }
}
