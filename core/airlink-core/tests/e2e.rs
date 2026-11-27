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
    // Neither node advertises or browses mDNS. The responder must retain the inbound
    // connection after pairing so it can send back across a one-way NAT.
    timeout(Duration::from_secs(5), async {
        while !b.reachable_trusted_peers().contains(&a.info().id) {
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

#[tokio::test]
async fn sender_giving_up_cancels_the_prompt() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", false).await;
    let b = node(&tmp.path().join("b"), "Beta", false).await;
    assert!(pair(&a, &b, true).await);
    std::fs::write(tmp.path().join("x.txt"), b"x").unwrap();

    let mut a_events = a.subscribe();
    let mut b_events = b.subscribe();
    let send = {
        let a = a.clone();
        let b_id = b.info().id.clone();
        let p = vec![tmp.path().join("x.txt")];
        tokio::spawn(async move { a.send_files(&b_id, &p).await })
    };
    // Sender sees "waiting for acceptance" right away.
    let pending_total = next_matching(&mut a_events, |e| match e {
        Event::TransferPending {
            bytes_total,
            file_count,
            ..
        } => Some((file_count, bytes_total)),
        _ => None,
    })
    .await;
    assert_eq!(pending_total, (1, 1));
    let req = next_matching(&mut b_events, |e| match e {
        Event::FileOffer { request_id, .. } => Some(request_id),
        _ => None,
    })
    .await;

    // Sender gives up before the receiver answers.
    send.abort();
    let cancelled = next_matching(&mut b_events, |e| match e {
        Event::RequestCancelled { request_id } => Some(request_id),
        _ => None,
    })
    .await;
    assert_eq!(cancelled, req);
    assert!(
        !b.respond(req, true),
        "a cancelled request can no longer be answered"
    );
}

/// Reproduces the PC -> phone bug: after `a` pairs with `b` and `b` sends to
/// `a`, `a` caches the connection `b` opened and reuses it to send back. `b`
/// opened that connection itself, so it must still accept requests on it.
#[tokio::test]
async fn requests_work_over_a_reused_connection_in_both_directions() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    assert!(pair(&a, &b, true).await);
    let f = tmp.path().join("f.txt");
    std::fs::write(&f, b"ping").unwrap();

    let wait = Duration::from_secs(10);
    // b -> a: b opens a fresh connection; a caches it as its route to b.
    timeout(wait, b.send_files(&a.info().id, std::slice::from_ref(&f)))
        .await
        .expect("b -> a timed out")
        .unwrap();
    // a -> b over the connection b opened (previously hung forever).
    timeout(wait, a.send_files(&b.info().id, std::slice::from_ref(&f)))
        .await
        .expect("a -> b over reused connection timed out")
        .unwrap();
    timeout(wait, a.send_clipboard(&b.info().id, "geri"))
        .await
        .expect("clipboard over reused connection timed out")
        .unwrap();
    assert!(tmp.path().join("b/downloads/f.txt").exists());
}

#[tokio::test]
async fn app_messages_reach_paired_devices_only() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    let c = node(&tmp.path().join("c"), "Gamma", true).await;
    assert!(pair(&a, &b, true).await);

    let mut b_events = b.subscribe();
    let body =
        serde_json::json!({"app": "WhatsApp", "title": "Ayşe", "text": "Akşam görüşelim mi?"});
    a.send_app_message(&b.info().id, "notification.posted", body.clone())
        .await
        .unwrap();
    let (kind, got, from) = next_matching(&mut b_events, |e| match e {
        Event::AppMessage { kind, body, peer } => Some((kind, body, peer.id)),
        _ => None,
    })
    .await;
    assert_eq!(kind, "notification.posted");
    assert_eq!(got, body);
    assert_eq!(from, a.info().id);

    // Not to unpaired devices, from either side.
    let err = a
        .send_app_message(&c.info().id, "x", serde_json::json!({}))
        .await
        .unwrap_err();
    assert!(format!("{err:#}").contains("not paired"));
}

#[tokio::test]
async fn media_frames_latest_wins() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    assert!(pair(&a, &b, true).await);
    let mut b_events = b.subscribe();
    let (a_id, b_id) = (a.info().id.clone(), b.info().id.clone());

    for i in 0..5u8 {
        a.send_media(&b_id, "screen", vec![i; 100_000]).unwrap();
    }
    next_matching(&mut b_events, |e| {
        matches!(e, Event::MediaStarted { ref name, .. } if name == "screen").then_some(())
    })
    .await;
    // Frames arrive; whatever was taken last is the newest one sent.
    let last = timeout(Duration::from_secs(5), async {
        loop {
            if let Some(f) = b.take_media(&a_id, "screen")
                && f[0] == 4
            {
                return f;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("the newest frame should arrive");
    assert_eq!(last.len(), 100_000);
    assert!(
        b.take_media(&a_id, "screen").is_none(),
        "taken frames are gone"
    );

    a.stop_media(&b_id, "screen");
    next_matching(&mut b_events, |e| {
        matches!(e, Event::MediaStopped { .. }).then_some(())
    })
    .await;
}

#[tokio::test]
async fn trusted_device_sends_without_a_prompt() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", false).await;
    let b = node(&tmp.path().join("b"), "Beta", false).await;
    assert!(pair(&a, &b, true).await);
    let a_id = a.info().id.clone();
    assert!(b.set_peer_auto_accept(&a_id, true).unwrap());
    assert!(!b.set_peer_auto_accept("not-paired", true).unwrap());

    // b trusts a: no FileOffer, the file is saved.
    let mut b_events = b.subscribe();
    std::fs::write(tmp.path().join("t.txt"), b"trusted").unwrap();
    a.send_files(&b.info().id, &[tmp.path().join("t.txt")]).await.unwrap();
    while let Ok(e) = b_events.try_recv() {
        assert!(!matches!(e, Event::FileOffer { .. }), "a trusted device must not prompt");
    }
    assert_eq!(std::fs::read(tmp.path().join("b/downloads/t.txt")).unwrap(), b"trusted");

    // Taking the mark back asks again.
    assert!(b.set_peer_auto_accept(&a_id, false).unwrap());
    let b_id = b.info().id.clone();
    let path = tmp.path().join("t.txt");
    let send = tokio::spawn(async move {
        let _ = a.send_files(&b_id, &[path]).await;
    });
    let req = next_matching(&mut b_events, |e| match e {
        Event::FileOffer { request_id, .. } => Some(request_id),
        _ => None,
    })
    .await;
    assert!(b.respond(req, false));
    let _ = send.await;

    // The mark is saved with the pairing.
    assert!(b.set_peer_auto_accept(&a_id, true).unwrap());
    b.shutdown().await;
    let b = node(&tmp.path().join("b"), "Beta", false).await;
    assert!(b.trusted_peers().iter().any(|p| p.id == a_id && p.auto_accept));
}

#[tokio::test]
async fn a_password_arrives_marked() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", false).await;
    let b = node(&tmp.path().join("b"), "Beta", false).await;
    assert!(pair(&a, &b, true).await);
    let mut b_events = b.subscribe();
    a.send_clipboard_marked(&b.info().id, "s3cret", true).await.unwrap();
    let (text, sensitive) = next_matching(&mut b_events, |e| match e {
        Event::ClipboardReceived { text, sensitive, .. } => Some((text, sensitive)),
        _ => None,
    })
    .await;
    assert_eq!((text.as_str(), sensitive), ("s3cret", true));
    a.send_clipboard(&b.info().id, "plain").await.unwrap();
    let sensitive = next_matching(&mut b_events, |e| match e {
        Event::ClipboardReceived { sensitive, .. } => Some(sensitive),
        _ => None,
    })
    .await;
    assert!(!sensitive);
}

/// Either side cancels a transfer under way: both see it as cancelled, nothing half-done is kept.
#[tokio::test]
async fn cancel_transfer_either_side() {
    let tmp = tempfile::tempdir().unwrap();
    let a = node(&tmp.path().join("a"), "Alpha", true).await;
    let b = node(&tmp.path().join("b"), "Beta", true).await;
    assert!(pair(&a, &b, true).await);
    timeout(Duration::from_secs(5), async {
        while !b.is_trusted(&a.info().id) {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    })
    .await
    .unwrap();

    // Large enough to be still going when the first progress event comes.
    let src = tmp.path().join("big.bin");
    std::fs::write(&src, vec![7u8; 256 * 1024 * 1024]).unwrap();
    let dl = tmp.path().join("b/downloads");

    for receiver_cancels in [true, false] {
        let (mut a_events, mut b_events) = (a.subscribe(), b.subscribe());
        let send = {
            let (a, b_id, p) = (a.clone(), b.info().id.clone(), vec![src.clone()]);
            tokio::spawn(async move { a.send_files(&b_id, &p).await })
        };
        let events = if receiver_cancels { &mut b_events } else { &mut a_events };
        let id = next_matching(events, |e| match e {
            Event::TransferProgress { transfer_id, bytes_done, .. } if bytes_done > 0 => Some(transfer_id),
            _ => None,
        })
        .await;
        let canceller = if receiver_cancels { &b } else { &a };
        assert!(canceller.cancel_transfer(&id));
        let err = send.await.unwrap().expect_err("a cancelled transfer fails").to_string();
        let expected = if receiver_cancels { airlink_core::TRANSFER_CANCELLED_BY_PEER } else { airlink_core::TRANSFER_CANCELLED };
        assert_eq!(err, expected);
        // The receiver hears it too, and keeps no file or .part.
        let b_error = next_matching(&mut b_events, |e| match e {
            Event::TransferFailed { error, .. } => Some(error),
            _ => None,
        })
        .await;
        let expected = if receiver_cancels { airlink_core::TRANSFER_CANCELLED } else { airlink_core::TRANSFER_CANCELLED_BY_PEER };
        assert_eq!(b_error, expected);
        let left: Vec<_> = std::fs::read_dir(&dl).map(|d| d.flatten().map(|e| e.file_name()).collect()).unwrap_or_default();
        assert!(left.is_empty(), "left behind: {left:?}");
        assert!(!a.cancel_transfer(&id), "a finished transfer can't be cancelled");
    }
}
