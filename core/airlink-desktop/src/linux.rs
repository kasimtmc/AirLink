//! Linux: one xdg-desktop-portal RemoteDesktop session with a ScreenCast stream of a monitor. The
//! desktop asks the user once; its restore token (kept in the data dir) skips the dialog later.
//! Frames come over PipeWire on a thread of their own; input goes through the portal.

use std::os::fd::OwnedFd;
use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, bail};
use ashpd::desktop::remote_desktop::{
    Axis, DeviceType, KeyState, NotifyPointerAxisOptions, RemoteDesktop, SelectDevicesOptions,
};
use ashpd::desktop::screencast::{CursorMode, Screencast, SelectSourcesOptions, SourceType};
use ashpd::desktop::{PersistMode, Session};
use pipewire as pw;
use pw::spa;
use tokio::sync::mpsc;

use crate::input::{Button, Input};
use crate::{Backend, FrameSlot, Options, RawFrame, Sink};

const TOKEN_FILE: &str = "desktop-share.token";

pub(crate) struct LinuxShare {
    input: mpsc::UnboundedSender<Input>,
    stop: mpsc::UnboundedSender<()>,
    pw_quit: pw::channel::Sender<()>,
}

impl Backend for LinuxShare {
    fn input(&self, input: Input) {
        let _ = self.input.send(input);
    }

    fn stop(&self) {
        let _ = self.stop.send(());
        let _ = self.pw_quit.send(());
    }
}

pub(crate) async fn start(
    options: &Options,
    slot: Arc<FrameSlot>,
    sink: Arc<dyn Sink>,
) -> anyhow::Result<(LinuxShare, u32, u32)> {
    let token_path = options.data_dir.join(TOKEN_FILE);
    let token = std::fs::read_to_string(&token_path)
        .ok()
        .filter(|t| !t.trim().is_empty());

    let remote = RemoteDesktop::new()
        .await
        .context("the desktop has no remote desktop portal")?;
    let screencast = Screencast::new()
        .await
        .context("the desktop has no screen cast portal")?;
    let session = remote.create_session(Default::default()).await?;
    remote
        .select_devices(
            &session,
            SelectDevicesOptions::default()
                .set_devices(DeviceType::KEYBOARD | DeviceType::POINTER)
                .set_persist_mode(PersistMode::ExplicitlyRevoked)
                .set_restore_token(token.as_deref().map(str::trim)),
        )
        .await?;
    screencast
        .select_sources(
            &session,
            SelectSourcesOptions::default()
                .set_cursor_mode(CursorMode::EMBEDDED)
                .set_sources(SourceType::MONITOR)
                .set_multiple(false),
        )
        .await?;
    let response = remote
        .start(&session, None, Default::default())
        .await?
        .response()
        .context("the share was not allowed on this computer")?;
    if let Some(t) = response.restore_token() {
        save_token(&token_path, t);
    }
    if !response.devices().contains(DeviceType::POINTER) {
        bail!("the desktop did not allow pointer control");
    }
    let stream = response
        .streams()
        .first()
        .cloned()
        .context("no screen was chosen")?;
    let (lw, lh) = stream.size().unwrap_or((0, 0));
    let node_id = stream.pipe_wire_node_id();
    let fd = screencast
        .open_pipe_wire_remote(&session, Default::default())
        .await?;

    let pw_quit = spawn_pipewire(fd, node_id, slot.clone(), sink.clone())?;
    let (input_tx, input_rx) = mpsc::unbounded_channel();
    let (stop_tx, stop_rx) = mpsc::unbounded_channel();
    let size = (f64::from(lw.max(1)), f64::from(lh.max(1)));
    tokio::spawn(drive(
        remote, session, node_id, size, input_rx, stop_rx, slot, sink,
    ));
    let share = LinuxShare {
        input: input_tx,
        stop: stop_tx,
        pw_quit,
    };
    Ok((share, lw.max(0) as u32, lh.max(0) as u32))
}

fn save_token(path: &Path, token: &str) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(path, token);
}

/// Plays input until stopped, the desktop closes the session, or the capture ends.
#[allow(clippy::too_many_arguments)]
async fn drive(
    remote: RemoteDesktop,
    session: Session<RemoteDesktop>,
    stream: u32,
    size: (f64, f64),
    mut input: mpsc::UnboundedReceiver<Input>,
    mut stop: mpsc::UnboundedReceiver<()>,
    slot: Arc<FrameSlot>,
    sink: Arc<dyn Sink>,
) {
    use futures_util::StreamExt;
    let mut closed = session.receive_closed().await.ok().map(Box::pin);
    let mut by_user = false;
    loop {
        tokio::select! {
            Some(i) = input.recv() => {
                if let Err(e) = play(&remote, &session, stream, size, i).await {
                    eprintln!("airlink: desktop input: {e}");
                }
            }
            _ = stop.recv() => break,
            _ = async {
                match closed.as_mut() {
                    Some(c) => { c.next().await; }
                    None => std::future::pending::<()>().await,
                }
            } => {
                by_user = true;
                break;
            }
            _ = tokio::time::sleep(std::time::Duration::from_millis(500)) => {
                if slot.closed() { break; }
            }
        }
    }
    let _ = session.close().await;
    if by_user && !slot.closed() {
        slot.close();
        sink.ended("user");
    }
}

async fn play(
    remote: &RemoteDesktop,
    session: &Session<RemoteDesktop>,
    stream: u32,
    (w, h): (f64, f64),
    input: Input,
) -> ashpd::Result<()> {
    let at = |x: f64, y: f64| {
        remote.notify_pointer_motion_absolute(session, stream, x * w, y * h, Default::default())
    };
    let button = |b: Button, s: KeyState| {
        remote.notify_pointer_button(session, button_code(b), s, Default::default())
    };
    let key =
        |sym: i32, s: KeyState| remote.notify_keyboard_keysym(session, sym, s, Default::default());
    match input {
        Input::Move { x, y } => at(x, y).await,
        Input::Button {
            button: b,
            down,
            x,
            y,
        } => {
            at(x, y).await?;
            button(b, state(down)).await
        }
        Input::Click {
            button: b,
            x,
            y,
            count,
        } => {
            at(x, y).await?;
            for _ in 0..count {
                button(b, KeyState::Pressed).await?;
                button(b, KeyState::Released).await?;
            }
            Ok(())
        }
        Input::Scroll { x, y, dx, dy } => {
            at(x, y).await?;
            let (sx, sy) = (dx.round() as i32, dy.round() as i32);
            if sx == 0 && sy == 0 {
                // Fractions of a notch (touchpads, flings): smooth scrolling, 10 px a notch.
                let finish = NotifyPointerAxisOptions::default().set_finish(true);
                return remote
                    .notify_pointer_axis(session, dx * 10.0, dy * 10.0, finish)
                    .await;
            }
            if sy != 0 {
                remote
                    .notify_pointer_axis_discrete(session, Axis::Vertical, sy, Default::default())
                    .await?;
            }
            if sx != 0 {
                remote
                    .notify_pointer_axis_discrete(session, Axis::Horizontal, sx, Default::default())
                    .await?;
            }
            Ok(())
        }
        Input::Key {
            key: name,
            down,
            modifiers,
        } => {
            let Some(sym) = keysym(&name) else {
                return Ok(());
            };
            if let Some(d) = down {
                return key(sym, state(d)).await;
            }
            let mods: Vec<i32> = modifiers.iter().filter_map(|m| keysym(m)).collect();
            for &m in &mods {
                key(m, KeyState::Pressed).await?;
            }
            key(sym, KeyState::Pressed).await?;
            key(sym, KeyState::Released).await?;
            for &m in mods.iter().rev() {
                key(m, KeyState::Released).await?;
            }
            Ok(())
        }
        Input::Text { text } => {
            for c in text.chars() {
                key(char_keysym(c), KeyState::Pressed).await?;
                key(char_keysym(c), KeyState::Released).await?;
            }
            Ok(())
        }
    }
}

fn state(down: bool) -> KeyState {
    if down {
        KeyState::Pressed
    } else {
        KeyState::Released
    }
}

/// evdev button codes, as the portal takes them.
fn button_code(b: Button) -> i32 {
    match b {
        Button::Left => 0x110,
        Button::Right => 0x111,
        Button::Middle => 0x112,
    }
}

/// X keysyms for the named keys (crate::input::KEYS) and single characters.
fn keysym(name: &str) -> Option<i32> {
    Some(match name {
        "Enter" => 0xff0d,
        "Backspace" => 0xff08,
        "Escape" => 0xff1b,
        "Tab" => 0xff09,
        "Delete" => 0xffff,
        "Space" => 0x20,
        "Left" => 0xff51,
        "Up" => 0xff52,
        "Right" => 0xff53,
        "Down" => 0xff54,
        "Home" => 0xff50,
        "End" => 0xff57,
        "PageUp" => 0xff55,
        "PageDown" => 0xff56,
        "Insert" => 0xff63,
        "Shift" => 0xffe1,
        "Control" => 0xffe3,
        "Alt" => 0xffe9,
        "Super" => 0xffeb,
        f if f.len() >= 2 && f.starts_with('F') => {
            let n: i32 = f[1..].parse().ok().filter(|n| (1..=12).contains(n))?;
            0xffbe + n - 1
        }
        s if s.chars().count() == 1 => char_keysym(s.chars().next()?),
        _ => return None,
    })
}

/// A character's keysym: Latin-1 as it is, the rest as Unicode keysyms.
fn char_keysym(c: char) -> i32 {
    match c {
        '\n' => 0xff0d,
        '\t' => 0xff09,
        c if (c as u32) < 0x100 => c as i32,
        c => 0x0100_0000 | c as i32,
    }
}

/// Reads the screen cast's PipeWire stream on a thread of its own (PipeWire's loop isn't Send).
/// Returns once the stream is connected; the sender stops it.
fn spawn_pipewire(
    fd: OwnedFd,
    node_id: u32,
    slot: Arc<FrameSlot>,
    sink: Arc<dyn Sink>,
) -> anyhow::Result<pw::channel::Sender<()>> {
    let (quit_tx, quit_rx) = pw::channel::channel::<()>();
    let (ready_tx, ready_rx) = std::sync::mpsc::channel::<Result<(), String>>();
    std::thread::Builder::new()
        .name("airlink-desktop-pipewire".into())
        .spawn(move || {
            let result = run_pipewire(fd, node_id, slot.clone(), quit_rx, &ready_tx);
            if let Err(e) = &result {
                let _ = ready_tx.send(Err(format!("{e:#}")));
            }
            // The capture ended on its own (an error, or the desktop stopped it).
            if !slot.closed() {
                slot.close();
                sink.ended(if result.is_ok() { "user" } else { "failed" });
            }
        })?;
    match ready_rx.recv_timeout(std::time::Duration::from_secs(5)) {
        Ok(Ok(())) => Ok(quit_tx),
        Ok(Err(e)) => bail!("screen capture failed: {e}"),
        Err(_) => bail!("screen capture did not start"),
    }
}

fn format_pod() -> anyhow::Result<Vec<u8>> {
    use spa::param::format::{FormatProperties, MediaSubtype, MediaType};
    use spa::param::video::VideoFormat;
    let format = spa::pod::object!(
        spa::utils::SpaTypes::ObjectParamFormat,
        spa::param::ParamType::EnumFormat,
        spa::pod::property!(FormatProperties::MediaType, Id, MediaType::Video),
        spa::pod::property!(FormatProperties::MediaSubtype, Id, MediaSubtype::Raw),
        spa::pod::property!(
            FormatProperties::VideoFormat,
            Choice,
            Enum,
            Id,
            VideoFormat::BGRx,
            VideoFormat::BGRx,
            VideoFormat::BGRA,
            VideoFormat::RGBx,
            VideoFormat::RGBA
        ),
        spa::pod::property!(
            FormatProperties::VideoSize,
            Choice,
            Range,
            Rectangle,
            spa::utils::Rectangle {
                width: 1920,
                height: 1080
            },
            spa::utils::Rectangle {
                width: 1,
                height: 1
            },
            spa::utils::Rectangle {
                width: 8192,
                height: 8192
            }
        ),
        spa::pod::property!(
            FormatProperties::VideoFramerate,
            Choice,
            Range,
            Fraction,
            spa::utils::Fraction { num: 30, denom: 1 },
            spa::utils::Fraction { num: 0, denom: 1 },
            spa::utils::Fraction { num: 240, denom: 1 }
        ),
    );
    let (cursor, _) = spa::pod::serialize::PodSerializer::serialize(
        std::io::Cursor::new(Vec::new()),
        &spa::pod::Value::Object(format),
    )
    .map_err(|e| anyhow::anyhow!("format: {e:?}"))?;
    Ok(cursor.into_inner())
}

fn run_pipewire(
    fd: OwnedFd,
    node_id: u32,
    slot: Arc<FrameSlot>,
    quit: pw::channel::Receiver<()>,
    ready: &std::sync::mpsc::Sender<Result<(), String>>,
) -> anyhow::Result<()> {
    use spa::param::video::{VideoFormat, VideoInfoRaw};
    pw::init();
    let mainloop = pw::main_loop::MainLoopRc::new(None)?;
    let context = pw::context::ContextRc::new(&mainloop, None)?;
    let core = context.connect_fd_rc(fd, None)?;
    let stream = pw::stream::StreamBox::new(
        &core,
        "airlink-desktop",
        pw::properties::properties! {
            *pw::keys::MEDIA_TYPE => "Video",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Screen",
        },
    )?;
    let on_error = mainloop.clone();
    let _listener = stream
        .add_local_listener_with_user_data(VideoInfoRaw::default())
        .param_changed(|_, info, id, param| {
            if let Some(param) = param
                && id == spa::param::ParamType::Format.as_raw()
            {
                let _ = info.parse(param);
            }
        })
        .state_changed(move |_, _, _, new| {
            if let pw::stream::StreamState::Error(e) = new {
                eprintln!("airlink: screen capture: {e}");
                on_error.quit();
            }
        })
        .process(move |stream, info| {
            let Some(mut buffer) = stream.dequeue_buffer() else {
                return;
            };
            let Some(data) = buffer.datas_mut().first_mut() else {
                return;
            };
            let size = info.size();
            let (w, h) = (size.width as usize, size.height as usize);
            let chunk = data.chunk();
            let stride = match chunk.stride() {
                s if s > 0 => s as usize,
                _ => w * 4,
            };
            let offset = chunk.offset() as usize;
            let rgb_order = matches!(info.format(), VideoFormat::RGBx | VideoFormat::RGBA);
            let Some(bytes) = data.data() else { return };
            let end = offset + stride * h;
            if w == 0 || h == 0 || bytes.len() < end {
                return;
            }
            let data = bytes[offset..end].to_vec();
            slot.put(RawFrame {
                width: w,
                height: h,
                stride,
                rgb_order,
                data,
            });
        })
        .register()?;

    let bytes = format_pod()?;
    let mut params = [spa::pod::Pod::from_bytes(&bytes).context("format pod")?];
    stream.connect(
        spa::utils::Direction::Input,
        Some(node_id),
        pw::stream::StreamFlags::AUTOCONNECT | pw::stream::StreamFlags::MAP_BUFFERS,
        &mut params,
    )?;

    let looper = mainloop.clone();
    let _quit = quit.attach(mainloop.loop_(), move |()| looper.quit());
    let _ = ready.send(Ok(()));
    mainloop.run();
    let _ = stream.disconnect();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keysyms() {
        assert_eq!(keysym("Enter"), Some(0xff0d));
        assert_eq!(keysym("F12"), Some(0xffc9));
        assert_eq!(keysym("F13"), None);
        assert_eq!(keysym("a"), Some(0x61));
        assert_eq!(char_keysym('ş'), 0x0100_015f);
        for k in crate::input::KEYS {
            assert!(keysym(k).is_some(), "{k}");
        }
    }
}
