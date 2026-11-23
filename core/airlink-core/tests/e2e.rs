//! Two nodes on localhost: pairing, file + folder transfer, clipboard, auth.

use std::net::SocketAddr;
use std::path::Path;
use std::time::Duration;

use airlink_core::{Event, Node, NodeConfig};
use tokio::sync::broadcast::Receiver;
use tokio::time::timeout;

async fn node(dir: &Path, name: &str, auto_accept: bool) -> Node {
    let mut cfg = NodeConfig::with_defaults(Some(dir.join("data"))).unwrap();
    cfg.device_name = name.into();
    cfg.port = None;
    cfg.download_dir = dir.join("downloads");
    cfg.auto_accept_files = auto_accept;
    cfg.advertise = false;
    cfg.browse = false;
    Node::start(cfg).await.unwrap()
}

fn local_addr(n: &Node) -> SocketAddr {
    SocketAddr::from(([127, 0, 0, 1], n.info().port))
}

async fn next_matching<T>(rx: &mut Receiver<Event>, mut f: impl FnMut(Event) -> Option<T>) -> T {
    timeout(Duration::from_secs(10), async {
        loop {
            if let Some(v) = f(rx.recv().await.unwrap()) {
                return v;
            }
        }
    })
    .await
    .expect("timed out waiting for event")
}

/// Pairs `a` -> `b`, answering on b's side with `b_accepts`.
async fn pair(a: &Node, b: &Node, b_accepts: bool) -> bool {
    let mut b_events = b.subscribe();
    let pairing = a.start_pairing(local_addr(b)).await.unwrap();
    let a_code = pairing.code().to_owned();
    let (req, b_code) = next_matching(&mut b_events, |e| match e {
        Event::PairingRequest {
            request_id, code, ..
        } => Some((request_id, code)),
        _ => None,
    })
    .await;
    assert_eq!(a_code, b_code, "both sides must show the same code");
    assert!(b.respond(req, b_accepts));
    pairing.confirm(true).await.unwrap()
}

#[tokio::test]
async fn pair_transfer_and_clipboard() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", false).await;
    let b = node(&tmp.path().join("b"), "Beta", false).await;

    assert!(pair(&a, &b, true).await);
    assert!(a.is_trusted(&b.info().id));
    // b stores trust when it receives a's confirm; give it a moment.
    timeout(Duration::from_secs(5), async {
        while !b.is_trusted(&a.info().id) {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await
    .unwrap();

    // A file and a folder, including a 3 MiB file to exercise chunking.
    let src = tmp.path().join("src");
    std::fs::create_dir_all(src.join("album/sub")).unwrap();
    std::fs::write(src.join("hello.txt"), b"merhaba AirLink").unwrap();
    let big: Vec<u8> = (0..3 * 1024 * 1024u32)
        .map(|i| (i * 31 % 251) as u8)
        .collect();
    std::fs::write(src.join("album/big.bin"), &big).unwrap();
    std::fs::write(src.join("album/sub/note.md"), b"# not").unwrap();

    let mut b_events = b.subscribe();
    let send = {
        let a = a.clone();
        let b_id = b.info().id.clone();
        let paths = vec![src.join("hello.txt"), src.join("album")];
        tokio::spawn(async move { a.send_files(&b_id, &paths).await })
    };
    let (req, total) = next_matching(&mut b_events, |e| match e {
        Event::FileOffer {
            request_id,
            total_size,
            files,
            ..
        } => {
            assert_eq!(files.len(), 3);
            Some((request_id, total_size))
        }
        _ => None,
    })
    .await;
    assert_eq!(total, 15 + big.len() as u64 + 5);
    b.respond(req, true);
    send.await.unwrap().expect("transfer should succeed");

    let dl = tmp.path().join("b/downloads");
    assert_eq!(
        std::fs::read(dl.join("hello.txt")).unwrap(),
        b"merhaba AirLink"
    );
    assert_eq!(std::fs::read(dl.join("album/big.bin")).unwrap(), big);
    assert_eq!(
        std::fs::read(dl.join("album/sub/note.md")).unwrap(),
        b"# not"
    );

    // Same file again gets a unique name.
    let send = {
        let a = a.clone();
        let b_id = b.info().id.clone();
        let p = vec![src.join("hello.txt")];
        tokio::spawn(async move { a.send_files(&b_id, &p).await })
    };
    let req = next_matching(&mut b_events, |e| match e {
        Event::FileOffer { request_id, .. } => Some(request_id),
        _ => None,
    })
    .await;
    b.respond(req, true);
    send.await.unwrap().unwrap();
    assert!(dl.join("hello (1).txt").exists());

    // Declined offer.
    let send = {
        let a = a.clone();
        let b_id = b.info().id.clone();
        let p = vec![src.join("hello.txt")];
        tokio::spawn(async move { a.send_files(&b_id, &p).await })
    };
    let req = next_matching(&mut b_events, |e| match e {
        Event::FileOffer { request_id, .. } => Some(request_id),
        _ => None,
    })
    .await;
    b.respond(req, false);
    let err = send.await.unwrap().unwrap_err();
    assert!(format!("{err:#}").contains("declined"));

    // Clipboard, both directions (b -> a reuses the incoming connection).
    let mut a_events = a.subscribe();
    a.send_clipboard(&b.info().id, "kopyalanan metin")
        .await
        .unwrap();
    let text = next_matching(&mut b_events, |e| match e {
        Event::ClipboardReceived { text, .. } => Some(text),
        _ => None,
    })
    .await;
    assert_eq!(text, "kopyalanan metin");
    b.send_clipboard(&a.info().id, "geri").await.unwrap();
    let text = next_matching(&mut a_events, |e| match e {
        Event::ClipboardReceived { text, .. } => Some(text),
        _ => None,
    })
    .await;
    assert_eq!(text, "geri");

    a.shutdown().await;
    b.shutdown().await;
}

#[tokio::test]
async fn rejected_pairing_is_not_trusted() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    assert!(!pair(&a, &b, false).await);
    assert!(!a.is_trusted(&b.info().id));
    assert!(!b.is_trusted(&a.info().id));
}

#[tokio::test]
async fn unpaired_device_cannot_send() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    let c = node(&tmp.path().join("c"), "Gamma", true).await;
    assert!(pair(&a, &b, true).await);

    // a refuses to send to a device it has not paired with.
    std::fs::write(tmp.path().join("x.txt"), b"x").unwrap();
    let err = a
        .send_files(&c.info().id, &[tmp.path().join("x.txt")])
        .await
        .unwrap_err();
    assert!(format!("{err:#}").contains("not paired"));

    // The identity survives restarts.
    let a_id = a.info().id.clone();
    a.shutdown().await;
    let a2 = node(&tmp.path().join("a"), "Alpha", true).await;
    assert_eq!(a2.info().id, a_id);
    assert!(a2.is_trusted(&b.info().id));
}
