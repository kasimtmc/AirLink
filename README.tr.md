# AirLink

[English](README.md) · **Türkçe**

Linux, Windows ve Android cihazlarınız arasında, kendi ağınızda, hesap ve bulut olmadan kesintisiz bağlantı:

- **Dosyalar**: eşleşmiş cihazlar arasında dosya ve klasör gönderin. Sürükle bırak, Android'de paylaşım menüsü ya da Explorer, Dolphin veya GNOME Dosyalar'daki sağ tık menüsü kullanılabilir.
- **Pano**: başka bir cihazın panosuna metin gönderin.
- **Bildirimler**: telefonunuzun bildirimleri bilgisayarda görünür ve oradan yanıtlanabilir.
- **SMS**: konuşmaları bilgisayarda okuyun, yeni mesaj yazın ve alıcıyı kişilerinizden seçin.
- **Aramalar**: gelen aramalar bilgisayarda görünür. Yanıtlayabilir, kapatabilir, bekletebilir ve sessize alabilirsiniz. Hoparlör, Bluetooth ve ahize arasında geçiş yapabilir, sesi ayarlayabilir, arama kaydını ve tuş takımını kullanabilirsiniz.
- **Ekran**: telefonun ekranı bilgisayarda, dokunma ve kaydırma ile.
- **Telefon uygulamaları bilgisayarda** (gelişmiş mod): telefonun uygulama listesinden herhangi bir uygulamayı seçip bilgisayarda kendi penceresinde açın. Fare ve klavyeyle kullanabilir, küçültebilir ve kapatabilirsiniz. Telefonun kendi ekranı başka bir şey gösterirken de çalışmaya devam eder.

Cihazlar birbirini mDNS ile bulur. İki cihazı, iki ekranda da görünen 6 haneli kodu karşılaştırarak bir kez eşleştirirsiniz. Sonrasında tüm trafik karşılıklı TLS ile QUIC üzerinden gider.

## Durum

| Faz | Kapsam | Durum |
|---|---|---|
| 1 | Keşif, güvenli eşleştirme, şifreli QUIC bağlantısı, dosya aktarımı, pano | ✅ |
| 2 | Bildirimler, SMS, kişiler, aramalar | ✅ |
| 3 | Telefon ekranı ve telefon uygulamaları bilgisayarda | ✅ (Android → PC) |
| 4 | Bilgisayarı telefondan kontrol, el hareketiyle aktarım, Bluetooth ile arama sesi | ⏳ |

Linux ve Android gerçek cihazlarda denendi. Windows sürümü çapraz derleniyor ve çalışıyor, ancak ağ tarafının gerçek bir Windows makinesinde denenmesi gerekiyor; Wine altında `quinn-udp` soketini açamıyor.

## Yapı

```
core/airlink-core   Rust çekirdek kütüphane: protokol, keşif, eşleştirme, aktarımlar, mesajlar, medya akışları
core/airlink-capi   Çekirdeğin C arayüzü (include/airlink.h); uygulama bunu kullanır
core/airlink-adb    Gelişmiş mod için Android kablosuz hata ayıklama istemcisi (eşleştirme, kabuk)
core/airlink-cli    Geliştirme ve test için komut satırı aracı
app/                Uygulama: Linux, Windows ve Android için tek C++20 kod tabanı, Kompoz arayüzü
app/android         Android Gradle projesi; gelişmiş modun yetkili yardımcısı da burada
app/packaging       Masaüstü entegrasyonu (menü girdisi, dosya yöneticisi menüleri)
docs/protocol.md    Ağ protokolü
```

Arayüz, C++ için Compose tarzı bir arayüz kütüphanesi olan [Kompoz](https://github.com/kasimtmc/kompoz) ile yazılıyor. Rust sürümü [KompozR](https://github.com/kasimtmc/KompozR) hazır olduğunda arayüz Rust'a taşınacak.

## Derleme

AirLink'in yanında `../kompoz` konumunda bir Kompoz kopyası gerekir. Başka bir yerdeyse `-DKOMPOZ_DIR=...` verin.

### Linux

```bash
cmake -S app -B app/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build app/build
```

```bash
./app/build/airlink
```

Uygulama menüsü girdisi, her dosya için "Birlikte aç" ve Dolphin ile GNOME Dosyalar menülerine "AirLink ile gönder" eklemek için:

```bash
app/packaging/linux/install.sh
```

Kaldırmak için `install.sh --uninstall` çalıştırın.

### Windows

Linux'tan MinGW-w64 ile çapraz derlenir. Sonuç, yalnızca Windows'un kendi DLL'lerine bağlı tek bir `.exe`'dir.

```bash
rustup target add x86_64-pc-windows-gnu
```

```bash
cmake -S app -B app/build-windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=../kompoz/cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build app/build-windows
```

`airlink.exe` dosyasını `fonts/` ve `licenses/` klasörleriyle birlikte dağıtın. İlk açılışta, yalnızca o kullanıcı için, Explorer'ın sağ tık menüsüne ve "Gönder" menüsüne "AirLink ile gönder" ekler. `airlink.exe --unregister` ikisini de kaldırır.

### Android

Gerekenler:

- NDK 29.0.14206865
- SDK CMake 4.1.2
- `cargo-ndk`
- Rust hedefi `aarch64-linux-android`

```bash
cd app/android && ./gradlew installDebug
```

İzinler telefonda, uygulamadaki ayrı kartlardan istenir:

- bildirim erişimi
- SMS ve kişiler
- telefon
- erişilebilirlik (paylaşılan ekranda dokunma için)

#### Gelişmiş mod

Telefon uygulamalarını bilgisayarda açmak, bir uygulamanın yapmasına izin verilenden fazlasını gerektirir: güvenilir sanal ekranlar oluşturmak ve girdi göndermek. AirLink bu yetkileri telefonun kendi **kablosuz hata ayıklama** özelliğinden alır. scrcpy gibi araçların yaptığı gibi, ama bilgisayar ya da kablo olmadan:

1. Telefonda *Geliştirici seçenekleri → Kablosuz hata ayıklama*'yı açın.
2. AirLink'te gelişmiş modu başlatın ve *Eşleme kodu ile cihaz eşle*'yi seçin.
3. Kodu AirLink'in bildirimine girin.

AirLink ardından kabuk kullanıcısı olarak küçük bir yardımcı başlatır. Yardımcı her uygulamayı kendi ekranında açar, karelerini gönderir ve girdilerinizi uygulamaya iletir. Yardımcıyla yalnızca AirLink konuşabilir; yardımcı yerel sokette bir belirteç doğrular. AirLink kapanınca yardımcı da durur.

## Komut satırı aracı

Derleyin:

```bash
cargo build --release
```

Bir cihazda çalıştırın. Pano eşitlemesi isteğe bağlıdır:

```bash
./target/release/airlink run --clipboard-sync
```

Başka bir cihazda ağdaki cihazları bulun:

```bash
./target/release/airlink discover
```

Sonra biriyle eşleşin:

```bash
./target/release/airlink pair "Cihaz adı"
```

İki cihaz da aynı 6 haneli kodu gösterir; ikisinde de `y` ile onaylayın. Eşleştikten sonra dosya ve klasör gönderebilirsiniz:

```bash
./target/release/airlink send "Cihaz adı" belge.pdf klasor/
```

Gelen dosyalar `~/Downloads/AirLink` klasörüne kaydedilir. Diğer komutlar için `airlink --help` çalıştırın: `info`, `peers`, `clip`, `msg` ve `forget`.

## Testler

```bash
cargo test
```

Uçtan uca testler gerçek düğümleri loopback üzerinden çalıştırır; eşleştirme, aktarımlar, yeniden bağlanma, uygulama mesajları ve medya akışlarını kapsar.

## Lisans

Henüz belirlenmedi.
