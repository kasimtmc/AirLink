//! Manual check on a real desktop: shares the screen for a few seconds, counts the encoded frames
//! and writes the last one to the given file (ALH1 + H.264). Linux asks the desktop the first time.
//!   cargo run -p airlink-desktop --example capture -- /tmp/frame.alh1

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

struct Count {
    frames: AtomicUsize,
    last: Mutex<Vec<u8>>,
}

impl airlink_desktop::Sink for Count {
    fn frame(&self, packet: Vec<u8>) {
        self.frames.fetch_add(1, Ordering::Relaxed);
        *self.last.lock().unwrap() = packet;
    }
    fn ended(&self, reason: &str) {
        eprintln!("ended: {reason}");
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let out = std::env::args().nth(1);
    let sink = Arc::new(Count {
        frames: AtomicUsize::new(0),
        last: Mutex::new(Vec::new()),
    });
    let dir = std::env::temp_dir().join("airlink-capture-example");
    let options = airlink_desktop::Options {
        data_dir: dir,
        ..Default::default()
    };
    let share = airlink_desktop::start(options, sink.clone()).await?;
    println!("sharing {:?}", share.size());
    tokio::time::sleep(std::time::Duration::from_secs(3)).await;
    share.stop();
    let last = sink.last.lock().unwrap().clone();
    println!(
        "frames: {}, last: {} bytes",
        sink.frames.load(Ordering::Relaxed),
        last.len()
    );
    if let Some(path) = out {
        std::fs::write(path, &last)?;
    }
    Ok(())
}
