//! This computer's screen and input, shared with a paired device: the screen is captured and
//! encoded as independent H.264 frames (`ALH1`, as the phone's screen), and the device's pointer,
//! wheel and keys are played here.
//!
//! - Linux: xdg-desktop-portal's RemoteDesktop session with a ScreenCast stream (the desktop asks
//!   the user once; the grant is kept with a restore token), frames over PipeWire.
//! - Windows: the primary monitor through GDI, input through SendInput.
//! - Elsewhere (Android): not available.

mod encoder;
mod input;
#[cfg(target_os = "linux")]
mod linux;
#[cfg(windows)]
mod windows;
#[cfg(windows)]
#[path = "bt_audio_windows.rs"]
pub mod bt_audio;

use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

pub use encoder::FrameEncoder;
pub use input::{Button, Input};

/// One captured picture: 32-bit pixels, blue first (BGRx/BGRA) unless `rgb_order`.
pub struct RawFrame {
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pub rgb_order: bool,
    pub data: Vec<u8>,
}

/// Where a share's output goes. Called on the share's own threads.
pub trait Sink: Send + Sync + 'static {
    /// An encoded frame (`ALH1` + Annex B), the newest one.
    fn frame(&self, packet: Vec<u8>);
    /// The share ended without `Share::stop` (the user stopped it on this computer, or it failed).
    fn ended(&self, reason: &str);
}

pub struct Options {
    /// Keeps the desktop's grant (Linux), so the user is asked only once.
    pub data_dir: PathBuf,
    /// Frames are scaled down to at most this width.
    pub max_width: usize,
    pub fps: u32,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            data_dir: PathBuf::from("."),
            max_width: 1280,
            fps: 12,
        }
    }
}

/// A running share. Dropping it stops it.
pub struct Share {
    width: u32,
    height: u32,
    backend: Box<dyn Backend>,
    slot: Arc<FrameSlot>,
}

impl Share {
    /// The captured screen's size, in pixels.
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn input(&self, input: Input) {
        self.backend.input(input);
    }

    pub fn stop(&self) {
        self.slot.close();
        self.backend.stop();
    }
}

impl Drop for Share {
    fn drop(&mut self) {
        self.stop();
    }
}

trait Backend: Send + Sync {
    fn input(&self, input: Input);
    fn stop(&self);
}

/// Whether this platform can share its screen.
pub fn supported() -> bool {
    cfg!(any(target_os = "linux", windows))
}

/// Starts sharing this computer's screen. Linux asks the desktop (a dialog, the first time).
pub async fn start(options: Options, sink: Arc<dyn Sink>) -> anyhow::Result<Share> {
    let slot = Arc::new(FrameSlot::default());
    let (backend, width, height): (Box<dyn Backend>, u32, u32) = {
        #[cfg(target_os = "linux")]
        {
            let (b, w, h) = linux::start(&options, slot.clone(), sink.clone()).await?;
            (Box::new(b), w, h)
        }
        #[cfg(windows)]
        {
            let (b, w, h) = windows::start(&options, slot.clone(), sink.clone())?;
            (Box::new(b), w, h)
        }
        #[cfg(not(any(target_os = "linux", windows)))]
        {
            let _ = (&options, &sink);
            anyhow::bail!("screen sharing is not available on this platform")
        }
    };
    spawn_encoder(slot.clone(), sink, options.max_width, options.fps);
    Ok(Share {
        width,
        height,
        backend,
        slot,
    })
}

/// The newest captured frame, between the capture and the encoder: a slow encoder skips frames
/// instead of queueing them.
#[derive(Default)]
pub(crate) struct FrameSlot {
    state: Mutex<(Option<RawFrame>, bool)>, // (frame, closed)
    wake: Condvar,
}

impl FrameSlot {
    pub(crate) fn put(&self, frame: RawFrame) {
        let mut s = self.state.lock().unwrap();
        if !s.1 {
            s.0 = Some(frame);
            self.wake.notify_one();
        }
    }

    pub(crate) fn close(&self) {
        let mut s = self.state.lock().unwrap();
        s.1 = true;
        s.0 = None;
        self.wake.notify_all();
    }

    pub(crate) fn closed(&self) -> bool {
        self.state.lock().unwrap().1
    }

    /// The next frame, or None once closed.
    fn take(&self) -> Option<RawFrame> {
        let mut s = self.state.lock().unwrap();
        loop {
            if s.1 {
                return None;
            }
            if let Some(f) = s.0.take() {
                return Some(f);
            }
            s = self.wake.wait(s).unwrap();
        }
    }
}

fn spawn_encoder(slot: Arc<FrameSlot>, sink: Arc<dyn Sink>, max_width: usize, fps: u32) {
    let interval = Duration::from_millis(1000 / u64::from(fps.clamp(1, 60)));
    std::thread::Builder::new()
        .name("airlink-desktop-encode".into())
        .spawn(move || {
            let mut encoder = None::<FrameEncoder>;
            while let Some(frame) = slot.take() {
                let began = Instant::now();
                let size = encoder::scaled_size(frame.width, frame.height, max_width);
                if encoder.as_ref().is_none_or(|e| e.size() != size) {
                    encoder = FrameEncoder::new(size.0, size.1).ok();
                }
                if let Some(e) = encoder.as_mut()
                    && let Some(packet) = e.encode(&frame)
                {
                    sink.frame(packet);
                }
                if let Some(rest) = interval.checked_sub(began.elapsed()) {
                    std::thread::sleep(rest);
                }
            }
        })
        .expect("spawn encoder thread");
}
