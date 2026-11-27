//! Refresh tokens: in the system keyring (Secret Service on Linux, so KWallet or GNOME Keyring;
//! Credential Manager on Windows) as "AirLink" / the account id. Where no keyring answers, in
//! `<data dir>/cloud/tokens.json`, readable only by this user.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use anyhow::Result;

const SERVICE: &str = "AirLink";

fn keyring(account: &str) -> Option<keyring::Entry> {
    keyring::Entry::new(SERVICE, account)
        .map_err(|e| tracing::warn!("keyring unavailable: {e}"))
        .ok()
}

fn file(dir: &Path) -> PathBuf {
    dir.join("tokens.json")
}

fn read_file(dir: &Path) -> BTreeMap<String, String> {
    std::fs::read(file(dir))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default()
}

fn write_file(dir: &Path, tokens: &BTreeMap<String, String>) -> Result<()> {
    let path = file(dir);
    if tokens.is_empty() {
        let _ = std::fs::remove_file(&path);
        return Ok(());
    }
    std::fs::create_dir_all(dir)?;
    let tmp = dir.join("tokens.json.tmp");
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
    use std::io::Write;
    options.open(&tmp)?.write_all(&serde_json::to_vec(tokens)?)?;
    std::fs::rename(&tmp, &path)?;
    Ok(())
}

pub(crate) fn save(dir: &Path, account: &str, token: &str) -> Result<()> {
    if let Some(entry) = keyring(account) {
        match entry.set_password(token) {
            Ok(()) => {
                // (a token kept in the file before a keyring was there goes)
                let mut tokens = read_file(dir);
                if tokens.remove(account).is_some() {
                    write_file(dir, &tokens)?;
                }
                return Ok(());
            }
            Err(e) => tracing::warn!("keyring: cannot save the sign-in, using a file: {e}"),
        }
    }
    let mut tokens = read_file(dir);
    tokens.insert(account.to_owned(), token.to_owned());
    write_file(dir, &tokens)
}

pub(crate) fn load(dir: &Path, account: &str) -> Option<String> {
    if let Some(entry) = keyring(account)
        && let Ok(token) = entry.get_password()
    {
        return Some(token);
    }
    read_file(dir).remove(account)
}

pub(crate) fn delete(dir: &Path, account: &str) {
    if let Some(entry) = keyring(account) {
        let _ = entry.delete_credential();
    }
    let mut tokens = read_file(dir);
    if tokens.remove(account).is_some() {
        let _ = write_file(dir, &tokens);
    }
}
