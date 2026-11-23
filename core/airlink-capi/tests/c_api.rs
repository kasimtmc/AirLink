//! Drives the C API the way the C++ app does: start, poll events, run ops.

use std::ffi::{CStr, CString, c_char};
use std::time::{Duration, Instant};

use airlink::*;
use serde_json::{Value, json};

fn take(s: *mut c_char) -> Option<Value> {
    if s.is_null() {
        return None;
    }
    let v = serde_json::from_str(unsafe { CStr::from_ptr(s) }.to_str().unwrap()).unwrap();
    unsafe { airlink_free_string(s) };
    Some(v)
}

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

struct N(*mut AirlinkNode);

impl N {
    fn start(dir: &std::path::Path, name: &str) -> N {
        let cfg = json!({"data_dir": dir.join("data"), "download_dir": dir.join("dl"), "device_name": name});
        let mut err: *mut c_char = std::ptr::null_mut();
        let n = unsafe { airlink_start(c(&cfg.to_string()).as_ptr(), &mut err) };
        assert!(!n.is_null(), "start failed: {:?}", take(err));
        N(n)
    }
    fn info(&self) -> Value {
        take(unsafe { airlink_info(self.0) }).unwrap()
    }
    /// Polls until `f` accepts an event.
    fn wait<T>(&self, mut f: impl FnMut(&Value) -> Option<T>) -> T {
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            while let Some(ev) = take(unsafe { airlink_poll_event(self.0) }) {
                if let Some(v) = f(&ev) {
                    return v;
                }
            }
            assert!(Instant::now() < deadline, "timed out waiting for event");
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    fn op(&self, op: u64) -> Value {
        assert_ne!(op, 0);
        self.wait(|e| (e["event"] == "op_result" && e["op"] == op).then(|| e.clone()))
    }
}

impl Drop for N {
    fn drop(&mut self) {
        unsafe { airlink_stop(self.0) };
    }
}

static HEARD: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());

extern "C" fn heard(_ctx: *mut std::ffi::c_void, json: *const c_char) {
    let v: Value = serde_json::from_str(unsafe { CStr::from_ptr(json) }.to_str().unwrap()).unwrap();
    HEARD
        .lock()
        .unwrap()
        .push(v["event"].as_str().unwrap().to_owned());
}

#[test]
fn pair_send_and_clipboard_through_the_c_api() {
    let tmp = tempfile::tempdir().unwrap();
    let a = N::start(&tmp.path().join("a"), "Alpha");
    let b = N::start(&tmp.path().join("b"), "Beta");
    let (a_id, b_id) = (
        a.info()["id"].as_str().unwrap().to_owned(),
        b.info()["id"].as_str().unwrap().to_owned(),
    );

    // mDNS finds b on the same machine.
    a.wait(|_| {
        None::<()>.or_else(|| {
            let peers = take(unsafe { airlink_peers(a.0) }).unwrap();
            peers
                .as_array()
                .unwrap()
                .iter()
                .any(|p| p["id"] == b_id.as_str())
                .then_some(())
        })
    });

    // b shows prompts outside its UI too (Android's notifications).
    unsafe { airlink_set_prompt_listener(b.0, Some(heard), std::ptr::null_mut()) };

    let op = unsafe { airlink_start_pairing(a.0, c(&b_id).as_ptr()) };
    let (pairing, code) = a.wait(|e| {
        if e["event"] == "op_result" && e["op"] == op {
            panic!("pairing failed: {e}");
        }
        (e["event"] == "pairing_code").then(|| (e["pairing"].as_u64().unwrap(), e["code"].clone()))
    });
    let (req, b_code) = b.wait(|e| {
        (e["event"] == "pairing_request")
            .then(|| (e["request_id"].as_u64().unwrap(), e["code"].clone()))
    });
    assert_eq!(code, b_code);
    assert!(unsafe { airlink_respond(b.0, req, true) });
    assert_eq!(
        *HEARD.lock().unwrap(),
        ["pairing_request", "request_answered"]
    );
    assert!(
        !unsafe { airlink_respond(b.0, req, true) },
        "answered once only"
    );
    let done = a.op(unsafe { airlink_confirm_pairing(a.0, pairing, true) });
    assert_eq!(done["value"], true, "{done}");

    let f = tmp.path().join("merhaba.txt");
    std::fs::write(&f, "selam").unwrap();
    let paths = json!([f]).to_string();
    let op = unsafe { airlink_send_files(a.0, c(&b_id).as_ptr(), c(&paths).as_ptr()) };
    let offer = b.wait(|e| (e["event"] == "file_offer").then(|| e["request_id"].as_u64().unwrap()));
    assert!(unsafe { airlink_respond(b.0, offer, true) });
    let res = a.op(op);
    assert_eq!(res["ok"], true, "{res}");
    assert_eq!(
        std::fs::read_to_string(tmp.path().join("b/dl/merhaba.txt")).unwrap(),
        "selam"
    );

    let res = b.op(unsafe { airlink_send_clipboard(b.0, c(&a_id).as_ptr(), c("pano").as_ptr()) });
    assert_eq!(res["ok"], true, "{res}");
    let text = a.wait(|e| (e["event"] == "clipboard_received").then(|| e["text"].clone()));
    assert_eq!(text, "pano");

    // Errors come back as failed op results, not crashes.
    let res = a.op(unsafe { airlink_send_clipboard(a.0, c("nope").as_ptr(), c("x").as_ptr()) });
    assert_eq!(res["ok"], false);
}
