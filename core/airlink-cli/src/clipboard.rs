//! Clipboard sync: polls the local clipboard on a dedicated thread (the OS
//! clipboard handle must stay alive and on one thread) and pushes changes to
//! reachable paired devices; applies incoming text locally.

use std::sync::mpsc as std_mpsc;
use std::time::Duration;

use airlink_core::Node;
use anyhow::Result;

pub struct ClipboardSync {
    to_local: std_mpsc::Sender<String>,
}

impl ClipboardSync {
    /// Applies text received from another device.
    pub fn set_remote(&self, text: String) {
        let _ = self.to_local.send(text);
    }
}

pub fn start(node: Node) -> Result<ClipboardSync> {
    let (to_local, from_remote) = std_mpsc::channel::<String>();
    let (changed_tx, mut changed_rx) = tokio::sync::mpsc::unbounded_channel::<String>();

    // Fail early if the clipboard is unavailable (e.g. no display server).
    arboard::Clipboard::new()?;

    std::thread::Builder::new()
        .name("airlink-clipboard".into())
        .spawn(move || {
            let mut cb = match arboard::Clipboard::new() {
                Ok(c) => c,
                Err(e) => {
                    eprintln!("clipboard unavailable: {e}");
                    return;
                }
            };
            let mut last = cb.get_text().unwrap_or_default();
            loop {
                while let Ok(text) = from_remote.try_recv() {
                    if cb.set_text(text.clone()).is_ok() {
                        last = text;
                    }
                }
                if let Ok(now) = cb.get_text()
                    && now != last
                    && !now.is_empty()
                {
                    last = now.clone();
                    if changed_tx.send(now).is_err() {
                        return;
                    }
                }
                std::thread::sleep(Duration::from_millis(500));
            }
        })?;

    tokio::spawn(async move {
        while let Some(text) = changed_rx.recv().await {
            for id in node.reachable_trusted_peers() {
                if let Err(e) = node.send_clipboard(&id, &text).await {
                    eprintln!(
                        "clipboard sync to {} failed: {e:#}",
                        airlink_core::short_id(&id)
                    );
                }
            }
        }
    });

    println!("Clipboard sync enabled.");
    Ok(ClipboardSync { to_local })
}
