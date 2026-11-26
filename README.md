# AirLink

**English** · [Türkçe](README.tr.md)

A seamless link between your Linux, Windows and Android devices, on your own network, with no account and no cloud:

- **Files**: send files and folders between any paired devices. Use drag and drop, the share sheet on Android, or the right-click menu in Explorer, Dolphin or GNOME Files.
- **Clipboard**: send text to another device's clipboard.
- **Notifications**: your phone's notifications appear on the computer, and you can reply to them from there.
- **SMS**: read conversations on the computer, write new messages and pick recipients from your contacts.
- **Calls**: incoming calls show on the computer. You can answer, hang up, hold and mute, switch between speaker, Bluetooth and earpiece, change the volume, and use the call log and dialer.
- **Phone audio**: share calls over Bluetooth HFP or player audio over Bluetooth A2DP with a paired Linux PC. Screen sharing audio uses its own stream on Android, Linux and Windows.
- **Screen**: the phone's screen on the computer, with touch and swipe.
- **Phone apps on the computer** (advanced mode): pick any app from the phone's app list and open it in its own window on the computer. You can use it with mouse and keyboard, minimize it and close it. It keeps running while the phone's own screen does something else.

Devices find each other with mDNS. You pair two devices once by comparing a 6-digit code shown on both screens. After that, all traffic goes over QUIC with mutual TLS.

## Status

| Phase | Scope | State |
|---|---|---|
| 1 | Discovery, secure pairing, encrypted QUIC connection, file transfer, clipboard | ✅ |
| 2 | Notifications, SMS, contacts, calls | ✅ |
| 3 | Phone screen and phone apps on the computer | ✅ (Android → PC) |
| 4 | Controlling the PC from the phone, motion transfer by pushing the phone, call audio over Bluetooth | ⏳ (PC control and motion transfer done; Linux call audio tested, Windows call audio pending) |
| 5 | UI improvements | ⏳ |

Linux, Windows and Android are tested on real devices, including automatic discovery and file transfer with Windows. The Android app is written in Kotlin.

Phone apps and screen sharing stream H.264 (Android MediaCodec, OpenH264 on the computer) and fall back to JPEG. See [development status](docs/development-status.md) for open items.

## Layout

```
core/airlink-core   Rust core library: protocol, discovery, pairing, transfers, messages, media streams
core/airlink-capi   C API over the core (include/airlink.h), used by the app
core/airlink-adb    Android wireless-debugging client for advanced mode (pairing, shell)
core/airlink-cli    Command-line tool for development and testing
app/                The app: one C++20 code base for Linux, Windows and Android, with a Kompoz UI
app/android         Android Gradle project (Kotlin, package com.plyg.airlink), including the privileged helper
app/packaging       Desktop integration (menu entry, file manager menus)
docs/protocol.md    Wire protocol
```

The UI is built with [Kompoz](https://github.com/kasimtmc/kompoz), a Compose-style UI toolkit for C++. Once [KompozR](https://github.com/kasimtmc/KompozR), the Rust port, is ready, the UI will move to Rust.

## Building

You need a Kompoz checkout next to AirLink at `../kompoz`. If it is somewhere else, pass `-DKOMPOZ_DIR=...`.

### Linux

Install the development packages for PipeWire and PulseAudio's simple API (`libpipewire-0.3-dev`, `libpulse-dev` on Debian-based systems), plus BlueZ, WirePlumber, `bluetoothctl` and `pactl` for call audio. See [phone audio](docs/hfp-audio.md).

```bash
cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build app/build
```

```bash
./app/build/airlink
```

To add an application menu entry, "Open with" for any file, and "AirLink ile gönder" (Send with AirLink) to the Dolphin and GNOME Files menus:

```bash
app/packaging/linux/install.sh
```

Run `install.sh --uninstall` to remove them again.

To start AirLink when you log in, turn on **Oturum açılınca başlat** (Start at login) in Settings. It adds `~/.config/autostart/dev.airlink.AirLink.desktop`, which starts AirLink with `--background`: the window stays closed and AirLink waits in the tray.

#### KDE Plasma panel widget

On Plasma 6, `install.sh` also installs the AirLink panel widget (`app/packaging/linux/plasmoid`, id `dev.airlink.panel`) when `kpackagetool6` is available. It puts AirLink on the panel as an icon of its own: a click opens AirLink's tray panel under the icon (or starts AirLink), and the icon takes the tray icon's colors (blue starting, green connected, orange no devices, grey paused or not running, red error), read from `$XDG_RUNTIME_DIR/airlink.status`.

The widget runs the binary `install.sh` was given (default `app/build/airlink`); pass another path to point it elsewhere, and run `install.sh` again after moving the build:

```bash
app/packaging/linux/install.sh /path/to/airlink
```

Then right-click the panel, choose **Add Widgets…** and drag **AirLink** onto it. If it isn't listed yet, restart the Plasma shell:

```bash
systemctl --user restart plasma-plasmashell
```

To package the widget as a `.plasmoid` file for another machine (the binary's path is filled in when it is packaged):

```bash
widget=$(mktemp -d) && cp -r app/packaging/linux/plasmoid/. "$widget" && \
  sed -i "s|@AIRLINK_BIN@|/path/to/airlink|" "$widget/contents/ui/main.qml" && \
  (cd "$widget" && python3 -m zipfile -c "$OLDPWD/AirLink.plasmoid" metadata.json contents) && rm -rf "$widget"
```

Install it with `kpackagetool6 --type Plasma/Applet --install AirLink.plasmoid` (`--upgrade` for a newer one), or in **Add Widgets… → Get New → Install Widget From Local File…**. `install.sh --uninstall` removes it, as does `kpackagetool6 --type Plasma/Applet --remove dev.airlink.panel`.

### Windows

Cross-compiled from Linux with MinGW-w64. The result is a single `.exe` that depends only on Windows' own DLLs.

```bash
rustup target add x86_64-pc-windows-gnu
```

```bash
cmake -S app -B app/build-windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=../kompoz/cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build app/build-windows
```

Ship `airlink.exe` together with its `fonts/` and `licenses/` folders. For virtual machines and Remote Desktop sessions without Vulkan, run `python3 app/packaging/windows/package.py` to create `AirLink-Windows-ANGLE.zip` with ANGLE's Direct3D 11 libraries and license; see [Windows test steps](docs/windows-test.md). If multicast discovery cannot cross the virtual network, use **IP adresiyle eşleştir** in the Devices tab and enter the other device's IP and AirLink port. On first start, AirLink adds "AirLink ile gönder" (Send with AirLink) to Explorer's right-click menu and to the "Send to" menu, for the current user only. **Oturum açılınca başlat** (Start at login) in Settings adds AirLink to the current user's `Run` key with `--background`, so it starts in the tray; the packaged install starts through its `airlink-pkg` alias. `airlink.exe --unregister` removes the menus and the `Run` entry.

### Android

Requirements:

- NDK 29.0.14206865
- SDK CMake 4.1.2
- `cargo-ndk`
- the Rust target `aarch64-linux-android`

```bash
cd app/android && ./gradlew installDebug
```

The Android app ID is `com.plyg.airlink`. An older `dev.airlink.airlink` install has a separate app identity; pair devices again if its local data is unavailable.

Permissions are asked for on the phone, each from its own card in the app:

- notification access
- SMS and contacts
- phone
- accessibility, for touch on the shared screen
- running without battery restrictions, so AirLink stays reachable with the screen off

AirLink starts in the background when the phone has started and after it was updated; turn off **Telefon açılınca başlat** (Start when the phone starts) in Settings to stop that.

#### Advanced mode

Opening phone apps on the computer needs more than an app is allowed to do: creating trusted virtual displays and injecting input. AirLink gets these rights from the phone's own **wireless debugging**, the same way tools like scrcpy do, but without a computer or a cable:

1. Enable *Developer options → Wireless debugging* on the phone.
2. In AirLink, start advanced mode and choose *Pair device with pairing code*.
3. Enter the code in AirLink's notification.

If AirLink cannot find the phone's own ADB mDNS service, enter the connection port shown on the
phone's *Wireless debugging* screen in AirLink's **Gelişmiş mod → Kablosuz hata ayıklama portu** field,
then tap **Dene**. During a new pairing, the notification also accepts `123456:PORT`, using the
pairing port shown beside the six-digit code. The connection and pairing ports are different.

AirLink then starts a small helper as the shell user. The helper opens each app on a display of its own, streams its frames and injects your input. Only AirLink can talk to the helper, which checks a token on a local socket. The helper stops when AirLink does.

## Command-line tool

Build it:

```bash
cargo build --release
```

Run a device. Clipboard sync is optional:

```bash
./target/release/airlink run --clipboard-sync
```

On another device, find devices on the network:

```bash
./target/release/airlink discover
```

Then pair with one of them:

```bash
./target/release/airlink pair "Device name"
```

Both devices show the same 6-digit code; confirm with `y` on each. After pairing you can send files and folders:

```bash
./target/release/airlink send "Device name" document.pdf folder/
```

Received files go to `~/Downloads/AirLink`. Run `airlink --help` for the other commands: `info`, `peers`, `clip`, `msg` and `forget`.

## Tests

```bash
cargo test
```

The end-to-end tests run real nodes over loopback and cover pairing, transfers, reconnects, app messages and media streams.

## License

AirLink's code is source-available under the [Sustainable Use License 1.0](LICENSE). It is not OSI open source because commercial redistribution is restricted:

- You may use and modify it for personal, noncommercial or your own internal business purposes.
- You may share original or modified copies free of charge for noncommercial purposes, with the license and modification notices.
- Commercial redistribution or offering AirLink to others as a paid product or service needs a separate written license from Kasim.
- Forks and pull requests are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md). Forks must use their own name and branding; see [TRADEMARKS.md](TRADEMARKS.md).

Third-party components keep their own licenses (Kompoz: Apache 2.0, OpenH264 and ANGLE: BSD, Rust crates: mostly MIT/Apache 2.0). For permission beyond the community license, contact [Kasim](https://github.com/kasimtmc).
