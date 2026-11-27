//! The app registrations AirLink signs in with, one per provider. They are public clients (PKCE,
//! no confidential secret); Google's desktop clients also carry a "secret" that Google does not
//! treat as one.
//!
//! Each comes from, first to last: `<data dir>/cloud/clients.json` (someone's own registration),
//! the environment at run time, the environment at build time, AirLink's own registration below:
//!
//! ```json
//! {"onedrive": "<application id>", "dropbox": "<app key>",
//!  "gdrive": {"id": "<client id>", "secret": "<client secret>"}}
//! ```
//!
//! `AIRLINK_ONEDRIVE_CLIENT_ID`, `AIRLINK_DROPBOX_CLIENT_ID`, `AIRLINK_GDRIVE_CLIENT_ID`,
//! `AIRLINK_GDRIVE_CLIENT_SECRET`.

use std::path::Path;

use serde::Deserialize;

use crate::Provider;

/// AirLink's own registrations (public: they show in the browser's address bar at sign-in).
const DROPBOX: &str = "qhltau20544zsm3";
const ONEDRIVE: &str = "adbb2297-142f-48bf-9d6d-86c878ac5b5d";

#[derive(Clone, Debug, Default)]
pub struct Client {
    pub id: String,
    pub secret: Option<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Clients {
    onedrive: Option<Client>,
    dropbox: Option<Client>,
    gdrive: Option<Client>,
}

#[derive(Deserialize, Default)]
struct File {
    onedrive: Option<String>,
    dropbox: Option<String>,
    gdrive: Option<GoogleFile>,
}

#[derive(Deserialize)]
struct GoogleFile {
    id: String,
    secret: String,
}

fn env(run: &str, built: Option<&'static str>) -> Option<String> {
    std::env::var(run)
        .ok()
        .or_else(|| built.map(str::to_owned))
        .map(|v| v.trim().to_owned())
        .filter(|v| !v.is_empty())
}

impl Clients {
    pub fn load(dir: &Path) -> Clients {
        let file: File = std::fs::read(dir.join("clients.json"))
            .ok()
            .and_then(|b| serde_json::from_slice(&b).ok())
            .unwrap_or_default();
        let plain = |id: Option<String>| id.filter(|v| !v.trim().is_empty()).map(|id| Client { id, secret: None });
        let onedrive = plain(file.onedrive).or_else(|| {
            let id = env("AIRLINK_ONEDRIVE_CLIENT_ID", option_env!("AIRLINK_ONEDRIVE_CLIENT_ID")).unwrap_or_else(|| ONEDRIVE.to_owned());
            Some(Client { id, secret: None })
        });
        let dropbox = plain(file.dropbox).or_else(|| {
            let id = env("AIRLINK_DROPBOX_CLIENT_ID", option_env!("AIRLINK_DROPBOX_CLIENT_ID")).unwrap_or_else(|| DROPBOX.to_owned());
            Some(Client { id, secret: None })
        });
        let gdrive = file
            .gdrive
            .filter(|g| !g.id.trim().is_empty())
            .map(|g| Client { id: g.id, secret: Some(g.secret) })
            .or_else(|| {
                let id = env("AIRLINK_GDRIVE_CLIENT_ID", option_env!("AIRLINK_GDRIVE_CLIENT_ID"))?;
                let secret = env("AIRLINK_GDRIVE_CLIENT_SECRET", option_env!("AIRLINK_GDRIVE_CLIENT_SECRET"));
                Some(Client { id, secret })
            });
        Clients { onedrive, dropbox, gdrive }
    }

    pub fn get(&self, provider: Provider) -> Option<Client> {
        match provider {
            Provider::Onedrive => self.onedrive.clone(),
            Provider::Dropbox => self.dropbox.clone(),
            Provider::Gdrive => self.gdrive.clone(),
        }
    }

    pub fn has(&self, provider: Provider) -> bool {
        self.get(provider).is_some()
    }
}
