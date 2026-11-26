//! The paired phone's Bluetooth audio on Windows.
//!
//! - Player audio: the PC becomes an A2DP sink for the phone through `AudioPlaybackConnection`
//!   (Windows 10 2004+). Windows plays the stream on the default output; nothing is copied here.
//! - Call audio: the PC becomes the phone's Hands-Free unit through `PhoneLineTransportDevice`
//!   (register + HFP service-level connection). Windows routes the SCO audio to the local
//!   speaker and microphone itself (`CanRouteToLocalDevice`). The API needs the restricted
//!   `phoneLineTransportManagement` capability, so it only works from the packaged install.
//!
//! Both run on their own threads; the app polls their status.

use std::sync::{Condvar, Mutex, Once, OnceLock};
use std::thread;
use std::time::{Duration, Instant};

use windows::ApplicationModel::Calls::{
    PhoneLineTransport, PhoneLineTransportDevice, TransportDeviceAudioRoutingStatus,
};
use windows::Devices::Enumeration::{DeviceAccessStatus, DeviceInformation};
use windows::Media::Audio::{
    AudioPlaybackConnection, AudioPlaybackConnectionOpenResultStatus, AudioPlaybackConnectionState,
};
use windows::core::HSTRING;

/// Keeps a multithreaded apartment alive, so every thread here may call WinRT.
fn init_winrt() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| unsafe {
        let _ = windows::Win32::System::Com::CoIncrementMTAUsage();
    });
}

/// A paired device whose name matches the phone, or the only one when none match.
fn find_device(selector: &HSTRING, phone: &str) -> windows::core::Result<Option<(HSTRING, String)>> {
    let found = DeviceInformation::FindAllAsyncAqsFilter(selector)?.join()?;
    let mut all = Vec::new();
    for info in found {
        all.push((info.Id()?, info.Name()?.to_string()));
    }
    let wanted = phone.trim().to_lowercase();
    let matching: Vec<_> = all.iter().filter(|(_, name)| name.trim().to_lowercase() == wanted).collect();
    if matching.len() == 1 {
        return Ok(Some(matching[0].clone()));
    }
    if matching.is_empty() && all.len() == 1 {
        return Ok(all.pop());
    }
    Ok(None)
}

// ---- Call audio (HFP Hands-Free) --------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum CallPhase {
    Off = 0,
    Connecting = 1,
    WaitingForRoute = 2,
    Active = 3,
    Error = 4,
}

struct CallState {
    phase: CallPhase,
    status: String,
    /// Bumped by start/stop; a worker whose generation is stale quits.
    generation: u64,
}

fn call_state() -> &'static Mutex<CallState> {
    static S: OnceLock<Mutex<CallState>> = OnceLock::new();
    S.get_or_init(|| {
        Mutex::new(CallState { phase: CallPhase::Off, status: String::new(), generation: 0 })
    })
}

fn set_call(generation: u64, phase: CallPhase, status: &str) -> bool {
    let mut s = call_state().lock().unwrap();
    if s.generation != generation {
        return false;
    }
    s.phase = phase;
    s.status = status.to_string();
    true
}

fn call_current(generation: u64) -> bool {
    call_state().lock().unwrap().generation == generation
}

/// Whether this Windows has the API at all (1903+). Access is checked when a call starts.
pub fn call_supported() -> bool {
    init_winrt();
    PhoneLineTransportDevice::GetDeviceSelectorForPhoneLineTransport(PhoneLineTransport::Bluetooth).is_ok()
}

pub fn call_start(phone: &str) {
    let generation = {
        let mut s = call_state().lock().unwrap();
        s.generation += 1;
        s.phase = CallPhase::Connecting;
        s.status = "Bluetooth telefona bağlanıyor".into();
        s.generation
    };
    let phone = phone.to_string();
    thread::spawn(move || {
        init_winrt();
        if let Err(e) = run_call(generation, &phone) {
            log(&format!("HFP: {e}"));
            set_call(generation, CallPhase::Error, &format!("Windows Bluetooth görüşme hatası: {}", e.message()));
        }
    });
}

pub fn call_stop() {
    let mut s = call_state().lock().unwrap();
    s.generation += 1;
    s.phase = CallPhase::Off;
    s.status.clear();
}

pub fn call_status() -> (CallPhase, String) {
    let s = call_state().lock().unwrap();
    (s.phase, s.status.clone())
}

fn run_call(generation: u64, phone: &str) -> windows::core::Result<()> {
    let selector = PhoneLineTransportDevice::GetDeviceSelectorForPhoneLineTransport(PhoneLineTransport::Bluetooth)?;
    let Some((id, name)) = find_device(&selector, phone)? else {
        set_call(generation, CallPhase::Error, "Windows'ta eşleşmiş Bluetooth telefon bulunamadı");
        return Ok(());
    };
    log(&format!("HFP: device {name}"));
    let Ok(device) = PhoneLineTransportDevice::FromId(&id) else {
        set_call(generation, CallPhase::Error, "Telefon açılamadı; AirLink'i paketli kurulumdan başlatın");
        return Ok(());
    };
    let access = device.RequestAccessAsync()?.join()?;
    if access != DeviceAccessStatus::Allowed {
        log(&format!("HFP: access {}", access.0));
        set_call(generation, CallPhase::Error, "Windows telefon hattı iznini vermedi; paketli kurulum gerekli");
        return Ok(());
    }
    if !device.IsRegistered()? {
        device.RegisterApp()?;
    }
    if !call_current(generation) {
        let _ = device.UnregisterApp();
        return Ok(());
    }
    if !device.ConnectAsync()?.join()? {
        set_call(generation, CallPhase::Error, "Telefonun Bluetooth görüşme bağlantısı kurulamadı");
        let _ = device.UnregisterApp();
        return Ok(());
    }
    set_call(generation, CallPhase::WaitingForRoute, "Telefonun Bluetooth görüşme ses yolu bekleniyor");
    // Windows 11 tells when the call's audio can play here; on older builds assume it once
    // the phone had time to switch its route.
    let started = Instant::now();
    let mut active = false;
    while call_current(generation) {
        let route = device.AudioRoutingStatus().ok();
        let ready = match route {
            Some(r) => r == TransportDeviceAudioRoutingStatus::CanRouteToLocalDevice,
            None => started.elapsed() > Duration::from_secs(3),
        };
        if ready != active {
            active = ready;
            if ready {
                set_call(generation, CallPhase::Active, "Görüşme sesi bilgisayarda");
            } else {
                set_call(generation, CallPhase::WaitingForRoute, "Telefonun Bluetooth görüşme ses yolu bekleniyor");
            }
        }
        if !active && started.elapsed() > Duration::from_secs(40) {
            let reason = if route == Some(TransportDeviceAudioRoutingStatus::CannotRouteToLocalDevice) {
                "Windows görüşme sesini bu bilgisayara yönlendiremiyor"
            } else {
                "Telefonun Bluetooth görüşme sesi açılmadı"
            };
            set_call(generation, CallPhase::Error, reason);
            break;
        }
        thread::sleep(Duration::from_millis(500));
    }
    // Without the registration the phone stops treating this PC as its hands-free unit, so
    // later calls stay on the phone.
    if let Err(e) = device.UnregisterApp() {
        log(&format!("HFP: unregister {e}"));
    }
    Ok(())
}

// ---- Player audio (A2DP sink) -----------------------------------------------------------------

struct Player {
    desired: String,
    status: String,
    running: bool,
}

fn player() -> &'static (Mutex<Player>, Condvar) {
    static P: OnceLock<(Mutex<Player>, Condvar)> = OnceLock::new();
    P.get_or_init(|| {
        (Mutex::new(Player { desired: String::new(), status: String::new(), running: false }), Condvar::new())
    })
}

/// Opens the phone's media stream on this PC while `phone` is set; empty closes it.
pub fn player_set_phone(phone: &str) {
    let (lock, wake) = player();
    let mut p = lock.lock().unwrap();
    if p.desired == phone {
        return;
    }
    p.desired = phone.to_string();
    p.status = if phone.is_empty() { String::new() } else { "Bluetooth medya bağlantısı kuruluyor".into() };
    if !phone.is_empty() && !p.running {
        p.running = true;
        thread::spawn(run_player);
    }
    wake.notify_all();
}

pub fn player_status() -> String {
    player().0.lock().unwrap().status.clone()
}

fn set_player_status(phone: &str, status: &str) {
    let mut p = player().0.lock().unwrap();
    if p.desired == phone {
        p.status = status.to_string();
    }
}

fn run_player() {
    init_winrt();
    let (lock, wake) = player();
    loop {
        let phone = {
            let mut p = lock.lock().unwrap();
            while p.desired.is_empty() {
                p = wake.wait(p).unwrap();
            }
            p.desired.clone()
        };
        let connection = match open_player(&phone) {
            Ok(Some(c)) => {
                set_player_status(&phone, "Bluetooth medya profili bağlandı");
                Some(c)
            }
            Ok(None) => None,
            Err(e) => {
                log(&format!("A2DP: {e}"));
                set_player_status(&phone, "Bluetooth medya bağlantısı kurulamadı");
                None
            }
        };
        // Hold it (or wait before retrying) until the wanted phone changes.
        let retry = Duration::from_secs(8);
        let mut p = lock.lock().unwrap();
        let deadline = Instant::now() + retry;
        while p.desired == phone {
            if connection.is_none() {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    break;
                }
                p = wake.wait_timeout(p, left).unwrap().0;
            } else {
                p = wake.wait_timeout(p, Duration::from_secs(2)).unwrap().0;
                let closed = connection
                    .as_ref()
                    .and_then(|c| c.State().ok())
                    .is_some_and(|s| s == AudioPlaybackConnectionState::Closed);
                if closed && p.desired == phone {
                    p.status = "Bluetooth medya bağlantısı koptu".into();
                    break;
                }
            }
        }
        drop(p);
        if let Some(c) = connection {
            let _ = c.Close();
        }
    }
}

fn open_player(phone: &str) -> windows::core::Result<Option<AudioPlaybackConnection>> {
    let selector = AudioPlaybackConnection::GetDeviceSelector()?;
    let Some((id, name)) = find_device(&selector, phone)? else {
        set_player_status(phone, "Windows'ta aynı adla eşlenmiş Bluetooth telefon bulunamadı");
        return Ok(None);
    };
    log(&format!("A2DP: device {name}"));
    let connection = AudioPlaybackConnection::TryCreateFromId(&id)?;
    connection.StartAsync()?.join()?;
    let result = connection.OpenAsync()?.join()?;
    let status = result.Status()?;
    if status != AudioPlaybackConnectionOpenResultStatus::Success {
        log(&format!("A2DP: open status {} ({:?})", status.0, result.ExtendedError().ok()));
        set_player_status(
            phone,
            if status == AudioPlaybackConnectionOpenResultStatus::DeniedBySystem {
                "Windows Bluetooth medya bağlantısına izin vermedi"
            } else {
                "Bluetooth medya bağlantısı kurulamadı"
            },
        );
        let _ = connection.Close();
        return Ok(None);
    }
    Ok(Some(connection))
}

/// To stderr, and to the diagnostic launcher's log (the app redirects only the C runtime's
/// stderr, which Rust does not write through).
fn log(line: &str) {
    eprintln!("airlink: {line}");
    if let Some(path) = std::env::var_os("AIRLINK_DIAG_LOG") {
        use std::io::Write;
        if let Ok(mut file) = std::fs::OpenOptions::new().append(true).create(true).open(path) {
            let _ = writeln!(file, "airlink: {line}");
        }
    }
}

