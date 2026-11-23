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
core/airlink-cli    Geliştirme ve test için komut satırı aracı
app/                Flutter arayüzü (Linux / Windows / Android)
app/rust            Flutter ↔ Rust köprüsü (flutter_rust_bridge)
docs/protocol.md    Protokol tanımı
```

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

## Masaüstü uygulaması (Flutter)

Gerekenler: Flutter SDK, Rust, `flutter_rust_bridge_codegen` 2.13.0.

```bash
cd app && flutter run -d linux
```

Rust API'si (`app/rust/src/api/`) değişirse Dart bağlantılarını yeniden üretin:

```bash
cd app && flutter_rust_bridge_codegen generate
```

## Lisans

Henüz belirlenmedi.
