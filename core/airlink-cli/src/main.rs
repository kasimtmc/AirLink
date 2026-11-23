//! `airlink`: command-line client for AirLink.

use std::collections::VecDeque;
use std::io::Write;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::Duration;

use airlink_core::{Event, Node, NodeConfig, short_id};
use anyhow::{Context, Result, bail};
use clap::{Parser, Subcommand};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::sync::mpsc;

#[cfg(feature = "clipboard")]
mod clipboard;

#[derive(Parser)]
#[command(
    name = "airlink",
    version,
    about = "AirLink: share files and clipboard between your devices"
)]
struct Cli {
    /// Directory for identity and paired devices (default: per-user data dir).
    #[arg(long, global = true)]
    data_dir: Option<PathBuf>,
    /// Device name shown to others (default: hostname).
    #[arg(long, global = true)]
    name: Option<String>,
    /// Print events as JSON lines (for scripting).
    #[arg(long, global = true)]
    json: bool,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Run in the foreground: be discoverable, receive files and pairing requests.
    Run {
        #[arg(long, default_value_t = airlink_core::DEFAULT_PORT)]
        port: u16,
        /// Where received files are saved (default: ~/Downloads/AirLink).
        #[arg(long)]
        download_dir: Option<PathBuf>,
        /// Accept incoming files from paired devices without asking.
        #[arg(long)]
        auto_accept: bool,
        /// Keep the clipboard in sync with reachable paired devices.
        #[arg(long)]
        clipboard_sync: bool,
    },
    /// Show this device's id, name and paths.
    Info,
    /// List AirLink devices on the local network.
    Discover {
        #[arg(long, default_value_t = 3)]
        secs: u64,
    },
    /// Pair with a device (by name, id prefix, or ip:port).
    Pair { target: String },
    /// Send files or folders to a paired device.
    Send {
        peer: String,
        #[arg(required = true)]
        paths: Vec<PathBuf>,
    },
    /// Send text to a paired device's clipboard (reads stdin if TEXT is omitted).
    Clip { peer: String, text: Option<String> },
    /// Send an app message (JSON body) to a paired device, e.g. for testing features.
    Msg {
        peer: String,
        kind: String,
        body: String,
    },
    /// List paired devices.
    Peers,
    /// Remove a paired device.
    Forget { peer: String },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env().unwrap_or_else(|_| "warn".into()),
        )
        .with_writer(std::io::stderr)
        .init();

    let mut cfg = NodeConfig::with_defaults(cli.data_dir.clone())?;
    if let Some(n) = &cli.name {
        cfg.device_name = n.clone();
    }

    match cli.cmd {
        Cmd::Run {
            port,
            download_dir,
            auto_accept,
            clipboard_sync,
        } => {
            cfg.port = Some(port);
            if let Some(d) = download_dir {
                cfg.download_dir = d;
            }
            cfg.auto_accept_files = auto_accept;
            run(cfg, clipboard_sync, cli.json).await
        }
        cmd => {
            // One-shot commands: random port, look but don't announce.
            cfg.port = None;
            cfg.advertise = false;
            let node = Node::start(cfg).await?;
            // Ctrl+C closes connections cleanly, so the other side cancels
            // its prompt immediately instead of waiting for a timeout.
            let res = tokio::select! {
                r = one_shot(&node, cmd) => r,
                _ = tokio::signal::ctrl_c() => Err(anyhow::anyhow!("cancelled")),
            };
            node.shutdown().await;
            res
        }
    }
}

async fn one_shot(node: &Node, cmd: Cmd) -> Result<()> {
    match cmd {
        Cmd::Info => {
            let i = node.info();
            println!("Name:      {}", i.name);
            println!("Device id: {}", i.id);
            println!("Platform:  {}", i.platform);
            println!("Data dir:  {}", node.config().data_dir.display());
            println!("Downloads: {}", node.config().download_dir.display());
        }
        Cmd::Discover { secs } => {
            tokio::time::sleep(Duration::from_secs(secs)).await;
            let peers = node.discovered_peers();
            if peers.is_empty() {
                println!(
                    "No devices found. Is `airlink run` active on the other device and on the same network?"
                );
            }
            for p in peers {
                let paired = if node.is_trusted(&p.id) { "paired" } else { "" };
                let addrs: Vec<String> = p.addrs.iter().map(|a| a.to_string()).collect();
                println!(
                    "{:<24} {:<9} {:<8} {:<22} {}",
                    p.name,
                    p.platform,
                    short_id(&p.id),
                    addrs.join(","),
                    paired
                );
            }
        }
        Cmd::Pair { target } => {
            let pairing = match target.parse::<SocketAddr>() {
                Ok(addr) => node.start_pairing(addr).await?,
                Err(_) => {
                    let id = wait_for_peer(node, &target, Duration::from_secs(4)).await?;
                    node.start_pairing_with(&id).await?
                }
            };
            let peer = pairing.peer().clone();
            println!(
                "Pairing with {} ({}, {})",
                peer.name,
                peer.platform,
                short_id(&peer.id)
            );
            println!();
            println!("    Code: {}", pairing.code());
            println!();
            let ok = ask("Does the same code appear on the other device? [y/n] ").await?;
            println!("Waiting for the other device to confirm...");
            if pairing.confirm(ok).await? {
                println!("✓ Paired with {}", peer.name);
            } else {
                bail!("pairing was rejected");
            }
        }
        Cmd::Send { peer, paths } => {
            let id = resolve_trusted(node, &peer).await?;
            let mut events = node.subscribe();
            let printer = tokio::spawn(async move {
                while let Ok(e) = events.recv().await {
                    if let Event::TransferProgress {
                        bytes_done,
                        bytes_total,
                        ..
                    } = e
                    {
                        print_progress("Sending", bytes_done, bytes_total);
                    }
                }
            });
            println!("Waiting for the receiver to accept...");
            let res = node.send_files(&id, &paths).await;
            printer.abort();
            println!();
            res?;
            println!("✓ Sent");
        }
        Cmd::Clip { peer, text } => {
            let id = resolve_trusted(node, &peer).await?;
            let text = match text {
                Some(t) => t,
                None => {
                    let mut s = String::new();
                    std::io::Read::read_to_string(&mut std::io::stdin(), &mut s)?;
                    s
                }
            };
            node.send_clipboard(&id, &text).await?;
            println!("✓ Clipboard sent");
        }
        Cmd::Msg { peer, kind, body } => {
            let id = resolve_trusted(node, &peer).await?;
            let body: serde_json::Value =
                serde_json::from_str(&body).context("body must be JSON")?;
            node.send_app_message(&id, &kind, body).await?;
            println!("✓ Sent {kind}");
        }
        Cmd::Peers => {
            let peers = node.trusted_peers();
            if peers.is_empty() {
                println!("No paired devices yet. Use `airlink pair <name>`.");
            }
            for p in peers {
                let addr = p.last_addr.map(|a| a.to_string()).unwrap_or_default();
                println!(
                    "{:<24} {:<9} {:<8} {}",
                    p.name,
                    p.platform,
                    short_id(&p.id),
                    addr
                );
            }
        }
        Cmd::Forget { peer } => {
            let id = node.find_peer(&peer).context("no such device")?;
            node.forget_peer(&id)?;
            println!("✓ Forgot {}", short_id(&id));
        }
        Cmd::Run { .. } => unreachable!(),
    }
    Ok(())
}

/// Looks a peer up by name/id, giving mDNS a few seconds to find it.
async fn wait_for_peer(node: &Node, query: &str, max: Duration) -> Result<String> {
    let deadline = tokio::time::Instant::now() + max;
    loop {
        if let Some(id) = node.find_peer(query) {
            return Ok(id);
        }
        if tokio::time::Instant::now() >= deadline {
            bail!("device {query:?} not found on the network (try ip:port)");
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
    }
}

async fn resolve_trusted(node: &Node, query: &str) -> Result<String> {
    let id = node
        .find_peer(query)
        .with_context(|| format!("no paired device matches {query:?}"))?;
    if !node.is_trusted(&id) {
        bail!("{query:?} is not paired yet; run `airlink pair {query}` first");
    }
    // Give discovery a moment so we use a fresh address if it changed.
    let deadline = tokio::time::Instant::now() + Duration::from_millis(1500);
    while tokio::time::Instant::now() < deadline
        && !node.discovered_peers().iter().any(|p| p.id == id)
    {
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    Ok(id)
}

async fn ask(prompt: &str) -> Result<bool> {
    print!("{prompt}");
    std::io::stdout().flush()?;
    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    let line = lines.next_line().await?.unwrap_or_default();
    Ok(is_yes(&line))
}

fn is_yes(s: &str) -> bool {
    matches!(s.trim().to_lowercase().as_str(), "y" | "yes" | "e" | "evet")
}

fn print_progress(label: &str, done: u64, total: u64) {
    let pct = if total == 0 {
        100.0
    } else {
        done as f64 * 100.0 / total as f64
    };
    print!(
        "\r{label}: {pct:5.1}%  {} / {}   ",
        human(done),
        human(total)
    );
    let _ = std::io::stdout().flush();
}

fn human(n: u64) -> String {
    const U: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut v = n as f64;
    let mut i = 0;
    while v >= 1024.0 && i < U.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{n} B")
    } else {
        format!("{v:.1} {}", U[i])
    }
}

async fn run(cfg: NodeConfig, clipboard_sync: bool, json: bool) -> Result<()> {
    let node = Node::start(cfg).await?;
    let info = node.info().clone();
    println!(
        "AirLink is running as \"{}\" ({})",
        info.name,
        short_id(&info.id)
    );
    println!(
        "Listening on UDP port {}. Files are saved to {}",
        info.port,
        node.config().download_dir.display()
    );
    println!("Press Ctrl+C to stop.\n");

    let mut events = node.subscribe();

    // stdin lines answer prompts in FIFO order.
    let (line_tx, mut line_rx) = mpsc::unbounded_channel::<String>();
    tokio::spawn(async move {
        let mut lines = BufReader::new(tokio::io::stdin()).lines();
        while let Ok(Some(l)) = lines.next_line().await {
            if line_tx.send(l).is_err() {
                break;
            }
        }
    });
    let mut prompts: VecDeque<u64> = VecDeque::new();

    #[cfg(feature = "clipboard")]
    let clip = if clipboard_sync {
        Some(clipboard::start(node.clone())?)
    } else {
        None
    };
    #[cfg(not(feature = "clipboard"))]
    if clipboard_sync {
        bail!("built without clipboard support");
    }

    loop {
        tokio::select! {
            _ = tokio::signal::ctrl_c() => break,
            Some(line) = line_rx.recv() => {
                if let Some(id) = prompts.pop_front() {
                    let yes = is_yes(&line);
                    node.respond(id, yes);
                    println!("{}", if yes { "→ accepted" } else { "→ rejected" });
                }
            }
            ev = events.recv() => {
                let ev = match ev {
                    Ok(e) => e,
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(_) => break,
                };
                if json {
                    println!("{}", serde_json::to_string(&ev)?);
                }
                match ev {
                    Event::PeerDiscovered { peer } if !json => {
                        let tag = if node.is_trusted(&peer.id) { " (paired)" } else { "" };
                        println!("• Found {} [{}]{tag}", peer.name, peer.platform);
                    }
                    Event::RequestCancelled { request_id } => {
                        if let Some(pos) = prompts.iter().position(|&id| id == request_id) {
                            prompts.remove(pos);
                            if !json {
                                println!("⨯ The other device cancelled the request; no answer needed.");
                            }
                        }
                    }
                    Event::PairingRequest { request_id, peer, code } => {
                        prompts.push_back(request_id);
                        println!("\n🔗 Pairing request from {} ({}, {})", peer.name, peer.platform, short_id(&peer.id));
                        println!("    Code: {code}");
                        println!("   Same code on the other device? [y/n]");
                    }
                    Event::PairingCompleted { peer, success } if !json => {
                        if success { println!("✓ Paired with {}", peer.name) } else { println!("✗ Pairing with {} failed", peer.name) }
                    }
                    Event::FileOffer { request_id, peer, files, total_size, .. } => {
                        prompts.push_back(request_id);
                        println!("\n📥 {} wants to send {} file(s), {}:", peer.name, files.len(), human(total_size));
                        for f in files.iter().take(10) {
                            println!("     {} ({})", f.path, human(f.size));
                        }
                        if files.len() > 10 {
                            println!("     … and {} more", files.len() - 10);
                        }
                        println!("   Accept? [y/n]");
                    }
                    Event::TransferProgress { direction: airlink_core::Direction::Incoming, bytes_done, bytes_total, .. } if !json => {
                        print_progress("Receiving", bytes_done, bytes_total);
                    }
                    Event::TransferCompleted { direction: airlink_core::Direction::Incoming, files, .. } if !json => {
                        println!("\n✓ Received {} file(s)", files.len());
                        for f in files.iter().take(10) {
                            println!("     {}", f.display());
                        }
                    }
                    Event::TransferFailed { error, .. } if !json => println!("\n✗ Transfer failed: {error}"),
                    Event::AppMessage { peer, kind, body } if !json => {
                        println!("✉ {} from {}: {}", kind, peer.name, body);
                    }
                    Event::ClipboardReceived { peer, text } => {
                        #[cfg(feature = "clipboard")]
                        if let Some(c) = &clip {
                            c.set_remote(text.clone());
                        }
                        if !json {
                            let preview: String = text.chars().take(60).collect();
                            println!("📋 Clipboard from {}: {preview}{}", peer.name, if text.chars().count() > 60 { "…" } else { "" });
                        }
                    }
                    _ => {}
                }
            }
        }
    }
    println!("\nStopping...");
    node.shutdown().await;
    Ok(())
}
