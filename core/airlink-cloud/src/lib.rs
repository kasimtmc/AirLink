//! Cloud drives on the computer: OneDrive (Microsoft Graph), Dropbox and Google Drive, each over
//! its own web API. The user signs in once per account in the browser (OAuth with PKCE and a
//! loopback redirect); the refresh token goes to the system keyring, the account list to
//! `<data dir>/cloud/accounts.json`.
//!
//! Every provider is reached through the same calls: list a folder, search, quota, download,
//! upload, new folder, rename and delete. An [`Entry`]'s `id` is what the provider names it by:
//! OneDrive's and Google Drive's item ids, a Dropbox path. The root folder is `""`.

mod clients;
mod dropbox;
mod gdrive;
mod http;
mod oauth;
mod onedrive;
mod secrets;

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};
use serde::{Deserialize, Serialize};

pub use clients::{Client, Clients};

/// A cloud drive service.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Provider {
    Onedrive,
    Dropbox,
    Gdrive,
}

impl Provider {
    pub const ALL: [Provider; 3] = [Provider::Onedrive, Provider::Dropbox, Provider::Gdrive];

    pub fn key(self) -> &'static str {
        match self {
            Provider::Onedrive => "onedrive",
            Provider::Dropbox => "dropbox",
            Provider::Gdrive => "gdrive",
        }
    }

    pub fn parse(s: &str) -> Option<Provider> {
        Provider::ALL.into_iter().find(|p| p.key() == s)
    }

    pub fn title(self) -> &'static str {
        match self {
            Provider::Onedrive => "OneDrive",
            Provider::Dropbox => "Dropbox",
            Provider::Gdrive => "Google Drive",
        }
    }
}

/// A connected account. `id` is `<provider>:<the provider's user id>`.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Account {
    pub id: String,
    pub provider: Provider,
    pub name: String,
    pub email: String,
}

/// A file or folder in a drive.
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Entry {
    pub id: String,
    pub name: String,
    pub folder: bool,
    pub size: u64,
    /// RFC 3339, or empty when the provider gives none.
    #[serde(default)]
    pub modified: String,
    /// Google Drive's MIME type for its own documents (they are exported to download), else empty.
    #[serde(default)]
    pub kind: String,
}

/// A drive's used and total space in bytes; `total` is 0 when there is no limit or it is unknown.
#[derive(Clone, Copy, Debug, Default, Serialize)]
pub struct Quota {
    pub used: u64,
    pub total: u64,
}

/// Hears a transfer's progress: bytes done, bytes in all.
pub type Progress = Arc<dyn Fn(u64, u64) + Send + Sync>;

struct Tokens {
    access: String,
    expires: Instant,
}

struct Session {
    account: Account,
    tokens: tokio::sync::Mutex<Option<Tokens>>,
}

/// The connected accounts and their sessions.
pub struct Cloud {
    dir: PathBuf,
    http: reqwest::Client,
    clients: Clients,
    sessions: Mutex<Vec<Arc<Session>>>,
}

impl Cloud {
    /// Loads the accounts saved under `data_dir/cloud`. Nothing is sent until a call needs it.
    pub fn new(data_dir: &Path) -> Result<Cloud> {
        let dir = data_dir.join("cloud");
        let clients = Clients::load(&dir);
        let accounts: Vec<Account> = std::fs::read(dir.join("accounts.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default();
        let sessions = accounts
            .into_iter()
            .map(|account| Arc::new(Session { account, tokens: tokio::sync::Mutex::new(None) }))
            .collect();
        Ok(Cloud { dir, http: http::client()?, clients, sessions: Mutex::new(sessions) })
    }

    /// The providers this build can sign in to (those with an app registration).
    pub fn providers(&self) -> Vec<Provider> {
        Provider::ALL.into_iter().filter(|p| self.clients.has(*p)).collect()
    }

    pub fn accounts(&self) -> Vec<Account> {
        self.sessions.lock().unwrap().iter().map(|s| s.account.clone()).collect()
    }

    /// Signs in to `provider` in the browser: `open` gets the sign-in page's address. Waits (up
    /// to five minutes) for the browser to come back, then saves the account. Signing in to an
    /// account again replaces its saved sign-in.
    pub async fn connect(&self, provider: Provider, open: impl FnOnce(String)) -> Result<Account> {
        let client = self.clients.get(provider).ok_or_else(|| anyhow!("{} bu sürümde etkin değil", provider.title()))?;
        let grant = oauth::sign_in(&self.http, provider, &client, open).await?;
        let refresh = grant.refresh.clone().ok_or_else(|| anyhow!("{} kalıcı oturum vermedi", provider.title()))?;
        let access = grant.access.clone();
        let account = match provider {
            Provider::Onedrive => onedrive::account(&self.http, &access).await?,
            Provider::Dropbox => dropbox::account(&self.http, &access).await?,
            Provider::Gdrive => gdrive::account(&self.http, &access).await?,
        };
        let (dir, id) = (self.dir.clone(), account.id.clone());
        blocking(move || secrets::save(&dir, &id, &refresh)).await?;
        let session = Arc::new(Session {
            account: account.clone(),
            tokens: tokio::sync::Mutex::new(Some(Tokens { access, expires: grant.expires })),
        });
        {
            let mut sessions = self.sessions.lock().unwrap();
            sessions.retain(|s| s.account.id != account.id);
            sessions.push(session);
        }
        self.save_accounts()?;
        Ok(account)
    }

    /// Forgets an account and its saved sign-in on this computer. (It talks to the keyring: call
    /// it off the async runtime's threads.)
    pub fn disconnect(&self, account: &str) -> Result<()> {
        self.sessions.lock().unwrap().retain(|s| s.account.id != account);
        secrets::delete(&self.dir, account);
        self.save_accounts()
    }

    /// A folder's files and folders, folders first, each part by name.
    pub async fn list(&self, account: &str, folder: &str) -> Result<Vec<Entry>> {
        let call = self.call(account)?;
        let mut entries = match call.provider() {
            Provider::Onedrive => onedrive::list(&call, folder).await?,
            Provider::Dropbox => dropbox::list(&call, folder).await?,
            Provider::Gdrive => gdrive::list(&call, folder).await?,
        };
        sort(&mut entries);
        Ok(entries)
    }

    /// Files and folders anywhere in the drive whose name matches `query`.
    pub async fn search(&self, account: &str, query: &str) -> Result<Vec<Entry>> {
        let call = self.call(account)?;
        let mut entries = match call.provider() {
            Provider::Onedrive => onedrive::search(&call, query).await?,
            Provider::Dropbox => dropbox::search(&call, query).await?,
            Provider::Gdrive => gdrive::search(&call, query).await?,
        };
        sort(&mut entries);
        Ok(entries)
    }

    pub async fn quota(&self, account: &str) -> Result<Quota> {
        let call = self.call(account)?;
        match call.provider() {
            Provider::Onedrive => onedrive::quota(&call).await,
            Provider::Dropbox => dropbox::quota(&call).await,
            Provider::Gdrive => gdrive::quota(&call).await,
        }
    }

    /// Downloads a file into `dir` (a name already there gets " (2)" and on). Google Drive's
    /// own documents come as Office files (or PNG for drawings). Returns the file's path.
    pub async fn download(&self, account: &str, entry: &Entry, dir: &Path, progress: Progress) -> Result<PathBuf> {
        if entry.folder {
            bail!("klasörler indirilemiyor");
        }
        let call = self.call(account)?;
        let (response, name) = match call.provider() {
            Provider::Onedrive => (onedrive::download(&call, &entry.id).await?, entry.name.clone()),
            Provider::Dropbox => (dropbox::download(&call, &entry.id).await?, entry.name.clone()),
            Provider::Gdrive => gdrive::download(&call, entry).await?,
        };
        http::save(response, dir, &name, entry.size, progress).await
    }

    /// Uploads a file into `folder`. A name already there is kept: the new file gets another.
    pub async fn upload(&self, account: &str, folder: &str, file: &Path, progress: Progress) -> Result<Entry> {
        let meta = tokio::fs::metadata(file).await.with_context(|| format!("{} okunamadı", file.display()))?;
        if meta.is_dir() {
            bail!("klasörler yüklenemiyor: {}", file.display());
        }
        let name = file
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or_else(|| anyhow!("dosya adı okunamadı: {}", file.display()))?
            .to_owned();
        let call = self.call(account)?;
        let source = http::Source { path: file.to_owned(), size: meta.len(), progress };
        match call.provider() {
            Provider::Onedrive => onedrive::upload(&call, folder, &name, source).await,
            Provider::Dropbox => dropbox::upload(&call, folder, &name, source).await,
            Provider::Gdrive => gdrive::upload(&call, folder, &name, source).await,
        }
    }

    pub async fn create_folder(&self, account: &str, parent: &str, name: &str) -> Result<Entry> {
        let call = self.call(account)?;
        match call.provider() {
            Provider::Onedrive => onedrive::create_folder(&call, parent, name).await,
            Provider::Dropbox => dropbox::create_folder(&call, parent, name).await,
            Provider::Gdrive => gdrive::create_folder(&call, parent, name).await,
        }
    }

    pub async fn rename(&self, account: &str, id: &str, name: &str) -> Result<Entry> {
        let call = self.call(account)?;
        match call.provider() {
            Provider::Onedrive => onedrive::rename(&call, id, name).await,
            Provider::Dropbox => dropbox::rename(&call, id, name).await,
            Provider::Gdrive => gdrive::rename(&call, id, name).await,
        }
    }

    /// Moves a file or folder to the drive's recycle bin (Dropbox keeps deleted files to restore).
    pub async fn delete(&self, account: &str, id: &str) -> Result<()> {
        let call = self.call(account)?;
        match call.provider() {
            Provider::Onedrive => onedrive::delete(&call, id).await,
            Provider::Dropbox => dropbox::delete(&call, id).await,
            Provider::Gdrive => gdrive::delete(&call, id).await,
        }
    }

    fn call(&self, account: &str) -> Result<Call<'_>> {
        let session = self
            .sessions
            .lock()
            .unwrap()
            .iter()
            .find(|s| s.account.id == account)
            .cloned()
            .ok_or_else(|| anyhow!("bağlı hesap yok: {account}"))?;
        Ok(Call { cloud: self, session })
    }

    fn save_accounts(&self) -> Result<()> {
        std::fs::create_dir_all(&self.dir)?;
        let accounts = self.accounts();
        let tmp = self.dir.join("accounts.json.tmp");
        std::fs::write(&tmp, serde_json::to_vec_pretty(&accounts)?)?;
        std::fs::rename(&tmp, self.dir.join("accounts.json"))?;
        Ok(())
    }

    /// A valid access token for the session, refreshed when it is about to run out (or `fresh`).
    async fn access(&self, session: &Session, fresh: bool) -> Result<String> {
        let mut tokens = session.tokens.lock().await;
        if !fresh
            && let Some(t) = tokens.as_ref()
            && t.expires > Instant::now() + Duration::from_secs(60)
        {
            return Ok(t.access.clone());
        }
        let provider = session.account.provider;
        let client = self.clients.get(provider).ok_or_else(|| anyhow!("{} bu sürümde etkin değil", provider.title()))?;
        let (dir, id) = (self.dir.clone(), session.account.id.clone());
        let refresh = blocking(move || secrets::load(&dir, &id))
            .await
            .ok_or_else(|| anyhow!("{} oturumu bulunamadı: hesabı yeniden bağlayın", provider.title()))?;
        let grant = oauth::refresh(&self.http, provider, &client, &refresh).await?;
        if let Some(new) = grant.refresh.as_deref()
            && new != refresh
        {
            let (dir, id, new) = (self.dir.clone(), session.account.id.clone(), new.to_owned());
            blocking(move || secrets::save(&dir, &id, &new)).await?;
        }
        let access = grant.access.clone();
        *tokens = Some(Tokens { access: grant.access, expires: grant.expires });
        Ok(access)
    }
}

/// One account's calls: the HTTP client and a token that is kept fresh.
pub(crate) struct Call<'a> {
    cloud: &'a Cloud,
    session: Arc<Session>,
}

impl Call<'_> {
    fn provider(&self) -> Provider {
        self.session.account.provider
    }

    pub(crate) fn http(&self) -> &reqwest::Client {
        &self.cloud.http
    }

    /// Sends a request built with a bearer token, refreshing the token once on a 401 and waiting
    /// out rate limits (429, 503 with Retry-After) a few times. Fails on any other error status.
    pub(crate) async fn send(&self, build: impl Fn(&reqwest::Client, &str) -> reqwest::RequestBuilder) -> Result<reqwest::Response> {
        let mut fresh = false;
        let mut waits = 0;
        loop {
            let token = self.cloud.access(&self.session, fresh).await?;
            let response = build(&self.cloud.http, &token).send().await?;
            let status = response.status();
            if status == reqwest::StatusCode::UNAUTHORIZED && !fresh {
                fresh = true;
                continue;
            }
            if (status == reqwest::StatusCode::TOO_MANY_REQUESTS || status == reqwest::StatusCode::SERVICE_UNAVAILABLE) && waits < 3 {
                waits += 1;
                let secs = response
                    .headers()
                    .get(reqwest::header::RETRY_AFTER)
                    .and_then(|v| v.to_str().ok())
                    .and_then(|v| v.parse::<u64>().ok())
                    .unwrap_or(2 * waits)
                    .min(30);
                tokio::time::sleep(Duration::from_secs(secs)).await;
                continue;
            }
            return http::check(response).await;
        }
    }

    pub(crate) async fn json(&self, build: impl Fn(&reqwest::Client, &str) -> reqwest::RequestBuilder) -> Result<serde_json::Value> {
        Ok(self.send(build).await?.json().await?)
    }
}

fn sort(entries: &mut [Entry]) {
    entries.sort_by_cached_key(|e| (!e.folder, e.name.to_lowercase()));
}


/// The keyring's calls block (D-Bus on Linux): off the runtime's worker threads.
async fn blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> T {
    tokio::task::spawn_blocking(f).await.expect("keyring call panicked")
}
