# AirLink

Linux, Windows ve Android cihazlar arasında kesintisiz bağlantı sağlar:

- dosya aktarımı
- pano paylaşımı
- telefon bildirimleri, SMS ve aramalar
- cihazdan cihaza uzaktan kontrol
- el hareketiyle aktarım

## Yol haritası

| Faz | İçerik | Durum |
|---|---|---|
| 1 | Keşif (mDNS), güvenli eşleştirme (SAS), şifreli QUIC bağlantısı, dosya aktarımı, pano | 🚧 Geliştiriliyor |
| 2 | Bildirimler, SMS, arama kaydı, kişiler, gelen arama | ⏳ |
| 3 | Android ekranını PC'den kontrol, PC'yi telefondan kontrol | ⏳ |
| 4 | El hareketiyle aktarım, tek uygulama akışı, Bluetooth ile arama sesi | ⏳ |

## Yapı

```
core/airlink-core   Rust çekirdek kütüphane (protokol, ağ, aktarım)
core/airlink-capi   Çekirdeğin C arayüzü (include/airlink.h)
core/airlink-cli    Geliştirme ve test için komut satırı aracı
app/                Kompoz (C++20) arayüzü: Linux, Windows, Android tek kod
app/android         Android Gradle projesi (KompozActivity üzerine MainActivity)
docs/protocol.md    Protokol tanımı
```

Arayüz [Kompoz](../kompoz) ile yazılıyor; KompozR tamamlandığında arayüz Rust'a taşınacak.

## Hızlı başlangıç (CLI)

Derleme:

```bash
cargo build --release
```

**1. cihaz:** servisi başlatın. Pano eşitlemesi isteğe bağlıdır.

```bash
./target/release/airlink run --clipboard-sync
```

**2. cihaz:** ağdaki cihazları bulun ve eşleşin.

```bash
./target/release/airlink discover
```

```bash
./target/release/airlink pair "Cihaz-Adı"
```

Eşleştirme sırasında iki ekranda da 6 haneli bir kod görünür. Kodlar aynıysa iki tarafta da `y` yazarak onaylayın.

Eşleştikten sonra dosya ya da klasör gönderebilirsiniz:

```bash
./target/release/airlink send "Cihaz-Adı" dosya.pdf klasor/
```

Gelen dosyalar varsayılan olarak `~/Downloads/AirLink` klasörüne kaydedilir.

## Uygulama (Kompoz)

Kompoz deposunun AirLink'in yanında (`../kompoz`) olması gerekir; başka yerdeyse `-DKOMPOZ_DIR=...`.

Masaüstü (Linux):

```bash
cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build app/build
./app/build/airlink
```

Windows (Linux'tan MinGW-w64 ile çapraz derleme; tek `.exe`, yalnızca Windows'un kendi DLL'leri):

```bash
rustup target add x86_64-pc-windows-gnu
cmake -S app -B app/build-windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=../kompoz/cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build app/build-windows
```

`airlink.exe` `fonts/` ve `licenses/` klasörleriyle birlikte dağıtılır. İlk açılışta Explorer'a "AirLink ile gönder"
(sağ tık) ve "Gönder" menüsüne kısayol ekler (yalnızca bu kullanıcı için); `airlink.exe --unregister` kaldırır.

Linux sağ tık menüsü (Dolphin, GNOME Files) ve uygulama menüsü girdisi:

```bash
app/packaging/linux/install.sh
```

Android (NDK 29.0.14206865, SDK CMake 4.1.2, `cargo-ndk`, Rust hedefi `aarch64-linux-android`):

```bash
cd app/android && ./gradlew installDebug
```

## Lisans

Henüz belirlenmedi.
