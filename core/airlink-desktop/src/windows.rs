//! Windows: the primary monitor through GDI (BitBlt into a DIB section, cursor drawn in), input
//! through SendInput. Works without a desktop grant; the app shows that the share is on.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::input::{Button, Input};
use crate::{Backend, FrameSlot, Options, RawFrame, Sink};

pub(crate) struct WindowsShare {
    running: Arc<AtomicBool>,
}

impl Backend for WindowsShare {
    fn input(&self, input: Input) {
        play(input);
    }

    fn stop(&self) {
        self.running.store(false, Ordering::Relaxed);
    }
}

fn screen_size() -> (i32, i32) {
    unsafe { (GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)) }
}

pub(crate) fn start(
    options: &Options,
    slot: Arc<FrameSlot>,
    sink: Arc<dyn Sink>,
) -> anyhow::Result<(WindowsShare, u32, u32)> {
    let (w, h) = screen_size();
    if w <= 0 || h <= 0 {
        anyhow::bail!("no screen to share");
    }
    let running = Arc::new(AtomicBool::new(true));
    let run = running.clone();
    let interval = Duration::from_millis(1000 / u64::from(options.fps.clamp(1, 60)));
    std::thread::Builder::new()
        .name("airlink-desktop-capture".into())
        .spawn(move || {
            while run.load(Ordering::Relaxed) && !slot.closed() {
                let began = Instant::now();
                match capture() {
                    Some(frame) => slot.put(frame),
                    // The secure desktop (UAC, the lock screen) can't be read; try again.
                    None => std::thread::sleep(Duration::from_millis(250)),
                }
                if let Some(rest) = interval.checked_sub(began.elapsed()) {
                    std::thread::sleep(rest);
                }
            }
            let _ = &sink; // ended by Share::stop only
        })?;
    Ok((WindowsShare { running }, w as u32, h as u32))
}

/// The primary monitor's pixels (BGRx), with the cursor drawn in.
fn capture() -> Option<RawFrame> {
    let (w, h) = screen_size();
    if w <= 0 || h <= 0 {
        return None;
    }
    unsafe {
        let screen = GetDC(std::ptr::null_mut());
        if screen.is_null() {
            return None;
        }
        let memory = CreateCompatibleDC(screen);
        let mut info: BITMAPINFO = std::mem::zeroed();
        info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h; // top-down rows
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        let mut bits: *mut core::ffi::c_void = std::ptr::null_mut();
        let bitmap = CreateDIBSection(
            screen,
            &info,
            DIB_RGB_COLORS,
            &mut bits,
            std::ptr::null_mut(),
            0,
        );
        let mut frame = None;
        if !memory.is_null() && !bitmap.is_null() && !bits.is_null() {
            let old = SelectObject(memory, bitmap as HGDIOBJ);
            if BitBlt(memory, 0, 0, w, h, screen, 0, 0, SRCCOPY | CAPTUREBLT) != 0 {
                draw_cursor(memory);
                GdiFlush();
                let len = (w * h * 4) as usize;
                let data = std::slice::from_raw_parts(bits as *const u8, len).to_vec();
                let stride = w as usize * 4;
                frame = Some(RawFrame {
                    width: w as usize,
                    height: h as usize,
                    stride,
                    rgb_order: false,
                    data,
                });
            }
            SelectObject(memory, old);
        }
        if !bitmap.is_null() {
            DeleteObject(bitmap as HGDIOBJ);
        }
        if !memory.is_null() {
            DeleteDC(memory);
        }
        ReleaseDC(std::ptr::null_mut(), screen);
        frame
    }
}

unsafe fn draw_cursor(dc: HDC) {
    unsafe {
        let mut ci: CURSORINFO = std::mem::zeroed();
        ci.cbSize = std::mem::size_of::<CURSORINFO>() as u32;
        if GetCursorInfo(&mut ci) == 0 || ci.flags & CURSOR_SHOWING == 0 || ci.hCursor.is_null() {
            return;
        }
        let mut icon: ICONINFO = std::mem::zeroed();
        let (mut hx, mut hy) = (0, 0);
        if GetIconInfo(ci.hCursor, &mut icon) != 0 {
            hx = icon.xHotspot as i32;
            hy = icon.yHotspot as i32;
            if !icon.hbmMask.is_null() {
                DeleteObject(icon.hbmMask as HGDIOBJ);
            }
            if !icon.hbmColor.is_null() {
                DeleteObject(icon.hbmColor as HGDIOBJ);
            }
        }
        let (x, y) = (ci.ptScreenPos.x, ci.ptScreenPos.y);
        DrawIconEx(
            dc,
            x - hx,
            y - hy,
            ci.hCursor,
            0,
            0,
            0,
            std::ptr::null_mut(),
            DI_NORMAL,
        );
    }
}

fn send(inputs: &[INPUT]) {
    if !inputs.is_empty() {
        unsafe {
            SendInput(
                inputs.len() as u32,
                inputs.as_ptr(),
                std::mem::size_of::<INPUT>() as i32,
            )
        };
    }
}

fn mouse(flags: u32, dx: i32, dy: i32, data: i32) -> INPUT {
    let mut i: INPUT = unsafe { std::mem::zeroed() };
    i.r#type = INPUT_MOUSE;
    i.Anonymous.mi = MOUSEINPUT {
        dx,
        dy,
        mouseData: data as u32,
        dwFlags: flags,
        time: 0,
        dwExtraInfo: 0,
    };
    i
}

/// A move to a point of the primary monitor: absolute coordinates are 0..65535 across it.
fn move_to(x: f64, y: f64) -> INPUT {
    let ax = (x * 65535.0).round() as i32;
    let ay = (y * 65535.0).round() as i32;
    mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, ax, ay, 0)
}

fn button_flags(b: Button, down: bool) -> u32 {
    match (b, down) {
        (Button::Left, true) => MOUSEEVENTF_LEFTDOWN,
        (Button::Left, false) => MOUSEEVENTF_LEFTUP,
        (Button::Right, true) => MOUSEEVENTF_RIGHTDOWN,
        (Button::Right, false) => MOUSEEVENTF_RIGHTUP,
        (Button::Middle, true) => MOUSEEVENTF_MIDDLEDOWN,
        (Button::Middle, false) => MOUSEEVENTF_MIDDLEUP,
    }
}

fn key_input(vk: u16, scan: u16, flags: u32) -> INPUT {
    let mut i: INPUT = unsafe { std::mem::zeroed() };
    i.r#type = INPUT_KEYBOARD;
    i.Anonymous.ki = KEYBDINPUT {
        wVk: vk,
        wScan: scan,
        dwFlags: flags,
        time: 0,
        dwExtraInfo: 0,
    };
    i
}

fn vk_key(vk: u16, down: bool) -> INPUT {
    let extended = matches!(
        vk,
        VK_LEFT
            | VK_RIGHT
            | VK_UP
            | VK_DOWN
            | VK_HOME
            | VK_END
            | VK_PRIOR
            | VK_NEXT
            | VK_INSERT
            | VK_DELETE
    );
    let mut flags = if down { 0 } else { KEYEVENTF_KEYUP };
    if extended {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }
    key_input(vk, 0, flags)
}

fn play(input: Input) {
    match input {
        Input::Move { x, y } => send(&[move_to(x, y)]),
        Input::Button { button, down, x, y } => {
            send(&[move_to(x, y), mouse(button_flags(button, down), 0, 0, 0)])
        }
        Input::Click {
            button,
            x,
            y,
            count,
        } => {
            let mut list = vec![move_to(x, y)];
            for _ in 0..count {
                list.push(mouse(button_flags(button, true), 0, 0, 0));
                list.push(mouse(button_flags(button, false), 0, 0, 0));
            }
            send(&list);
        }
        Input::Scroll { x, y, dx, dy } => {
            let mut list = vec![move_to(x, y)];
            // WHEEL_DELTA (120) a notch; positive wheel data scrolls up on Windows.
            if dy != 0.0 {
                list.push(mouse(MOUSEEVENTF_WHEEL, 0, 0, (-dy * 120.0).round() as i32));
            }
            if dx != 0.0 {
                list.push(mouse(MOUSEEVENTF_HWHEEL, 0, 0, (dx * 120.0).round() as i32));
            }
            send(&list);
        }
        Input::Key {
            key,
            down,
            modifiers,
        } => {
            let Some(vk) = vk_code(&key) else {
                if down.is_none() && key.chars().count() == 1 {
                    play(Input::Text { text: key });
                }
                return;
            };
            if let Some(d) = down {
                return send(&[vk_key(vk, d)]);
            }
            let mods: Vec<u16> = modifiers.iter().filter_map(|m| vk_code(m)).collect();
            let mut list: Vec<INPUT> = mods.iter().map(|&m| vk_key(m, true)).collect();
            list.push(vk_key(vk, true));
            list.push(vk_key(vk, false));
            list.extend(mods.iter().rev().map(|&m| vk_key(m, false)));
            send(&list);
        }
        Input::Text { text } => {
            // Any language: UTF-16 units as Unicode key strokes.
            let mut list = Vec::new();
            for unit in text.encode_utf16() {
                if unit == u16::from(b'\n') {
                    list.push(vk_key(VK_RETURN, true));
                    list.push(vk_key(VK_RETURN, false));
                    continue;
                }
                list.push(key_input(0, unit, KEYEVENTF_UNICODE));
                list.push(key_input(0, unit, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP));
            }
            send(&list);
        }
    }
}

/// Virtual-key codes for the named keys (crate::input::KEYS).
fn vk_code(name: &str) -> Option<u16> {
    Some(match name {
        "Enter" => VK_RETURN,
        "Backspace" => VK_BACK,
        "Escape" => VK_ESCAPE,
        "Tab" => VK_TAB,
        "Delete" => VK_DELETE,
        "Space" => VK_SPACE,
        "Left" => VK_LEFT,
        "Right" => VK_RIGHT,
        "Up" => VK_UP,
        "Down" => VK_DOWN,
        "Home" => VK_HOME,
        "End" => VK_END,
        "PageUp" => VK_PRIOR,
        "PageDown" => VK_NEXT,
        "Insert" => VK_INSERT,
        "Shift" => VK_SHIFT,
        "Control" => VK_CONTROL,
        "Alt" => VK_MENU,
        "Super" => VK_LWIN,
        f if f.len() >= 2 && f.starts_with('F') => {
            let n: u16 = f[1..].parse().ok().filter(|n| (1..=12).contains(n))?;
            VK_F1 + n - 1
        }
        s if s.len() == 1 && s.as_bytes()[0].is_ascii_alphanumeric() => {
            u16::from(s.as_bytes()[0].to_ascii_uppercase())
        }
        _ => return None,
    })
}
