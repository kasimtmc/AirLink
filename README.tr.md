# AirLink

[English](README.md) · **Türkçe**

Linux, Windows ve Android cihazlarınız arasında, kendi ağınızda, hesap ve bulut olmadan kesintisiz bağlantı:

- **Dosyalar**: eşleşmiş cihazlar arasında dosya ve klasör gönderin. Sürükle bırak, Android'de paylaşım menüsü ya da Explorer, Dolphin veya GNOME Dosyalar'daki sağ tık menüsü kullanılabilir.
- **Pano**: başka bir cihazın panosuna metin gönderin.
- **Bildirimler**: telefonunuzun bildirimleri bilgisayarda görünür ve oradan yanıtlanabilir.
- **SMS**: konuşmaları bilgisayarda okuyun, yeni mesaj yazın ve alıcıyı kişilerinizden seçin.
- **Aramalar**: gelen aramalar bilgisayarda görünür. Yanıtlayabilir, kapatabilir, bekletebilir ve sessize alabilirsiniz. Hoparlör, Bluetooth ve ahize arasında geçiş yapabilir, sesi ayarlayabilir, arama kaydını ve tuş takımını kullanabilirsiniz.
- **Telefon sesi**: Eşleşmiş Linux bilgisayara görüşme sesini Bluetooth HFP, oynatıcı sesini Bluetooth A2DP üzerinden aktarın. Ekran paylaşımı sesi Android, Linux ve Windows arasında ayrı akışta taşınır.
- **Pil ve telefonu bul**: telefonun pil seviyesi ve şarj durumu bilgisayarda görünür; **Telefonu bul** telefonu sessiz kipte bile tam sesle çaldırır.
- **Ekran**: telefonun ekranı bilgisayarda, dokunma ve kaydırma ile.
- **Bulut sürücüleri** (Linux ve Windows): OneDrive, Dropbox ve Google Drive hesaplarının her biri kendi sekmesinde açılır. Gezinip arayabilir, indirip yükleyebilir, klasör oluşturabilir, yeniden adlandırıp silebilir ve bir dosyayı doğrudan eşleşmiş bir cihaza gönderebilirsiniz. Bkz. [bulut sürücüleri](docs/cloud-drives.md).
- **Telefon uygulamaları bilgisayarda** (gelişmiş mod): telefonun uygulama listesinden herhangi bir uygulamayı seçip bilgisayarda kendi penceresinde açın. Fare ve klavyeyle kullanabilir, küçültebilir ve kapatabilirsiniz. Telefonun kendi ekranı başka bir şey gösterirken de çalışmaya devam eder.

Cihazlar birbirini mDNS ile bulur. İki cihazı, iki ekranda da görünen 6 haneli kodu karşılaştırarak bir kez eşleştirirsiniz. Sonrasında tüm trafik karşılıklı TLS ile QUIC üzerinden gider.

## Durum

| Faz | Kapsam | Durum |
|---|---|---|
| 1 | Keşif, güvenli eşleştirme, şifreli QUIC bağlantısı, dosya aktarımı, pano | ✅ |
| 2 | Bildirimler, SMS, kişiler, aramalar | ✅ |
| 3 | Telefon ekranı ve telefon uygulamaları bilgisayarda | ✅ (Android → PC) |
| 4 | Bilgisayarı telefondan kontrol, telefonu iterek aktarım, Bluetooth ile arama sesi | ⏳ (bilgisayar kontrolü ve telefonu iterek aktarım tamam; Linux arama sesi denendi, Windows arama sesi bekliyor) |
| 5 | Arayüz iyileştirmeleri | ⏳ |

Linux, Windows ve Android gerçek cihazlarda denendi; Windows'la otomatik keşif ve dosya aktarımı da buna dahil. Android uygulaması Kotlin ile yazıldı.

Telefon uygulamaları ve ekran paylaşımı H.264 ile akar (Android'de MediaCodec, bilgisayarda OpenH264); gerekirse JPEG'e döner. Açık işler için [geliştirme durumu](docs/development-status.md) belgesine bakın.

## İndirme

Linux (AppImage, .deb, Flatpak, tar.gz), Windows (kurulum dosyası, taşınabilir zip) ve Android (APK) derlemeleri [Sürümler](https://github.com/kasimtmc/AirLink/releases) sayfasındadır. Nasıl hazırlandıkları için bkz. [sürüm çıkarma](docs/releasing.md).

## Yapı

```
core/airlink-core   Rust çekirdek kütüphane: protokol, keşif, eşleştirme, aktarımlar, mesajlar, medya akışları
core/airlink-capi   Çekirdeğin C arayüzü (include/airlink.h); uygulama bunu kullanır
core/airlink-adb    Gelişmiş mod için Android kablosuz hata ayıklama istemcisi (eşleştirme, kabuk)
core/airlink-cloud  Bulut sürücüleri: OneDrive, Dropbox ve Google Drive, web API'leri üzerinden
core/airlink-cli    Geliştirme ve test için komut satırı aracı
app/                Uygulama: Linux, Windows ve Android için tek C++20 kod tabanı, Kompoz arayüzü
app/android         Android Gradle projesi (Kotlin, paket com.plyg.airlink); gelişmiş modun yetkili yardımcısı da burada
app/packaging       Masaüstü entegrasyonu (menü girdisi, dosya yöneticisi menüleri)
docs/protocol.md    Ağ protokolü
```

Arayüz, C++ için Compose tarzı bir arayüz kütüphanesi olan [Kompoz](https://github.com/kasimtmc/kompoz) ile yazılıyor. Rust sürümü [KompozR](https://github.com/kasimtmc/KompozR) hazır olduğunda arayüz Rust'a taşınacak.

## Derleme

AirLink'in yanında `../kompoz` konumunda bir Kompoz kopyası gerekir. Başka bir yerdeyse `-DKOMPOZ_DIR=...` verin.

### Linux

PipeWire ve PulseAudio simple API geliştirme paketlerini kurun (Debian tabanlı sistemlerde `libpipewire-0.3-dev`, `libpulse-dev`). Arama sesi için ayrıca BlueZ, WirePlumber, `bluetoothctl` ve `pactl` gerekir. Bkz. [telefon sesi](docs/hfp-audio.md).

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

Oturum açınca AirLink'in başlaması için Ayarlar'da **Oturum açılınca başlat** anahtarını açın. Bu, AirLink'i `--background` ile başlatan `~/.config/autostart/dev.airlink.AirLink.desktop` dosyasını ekler: pencere açılmaz, AirLink sistem tepsisinde bekler.

#### KDE Plasma panel bileşeni

Plasma 6'da `kpackagetool6` varsa `install.sh` AirLink panel bileşenini de kurar (`app/packaging/linux/plasmoid`, kimliği `dev.airlink.panel`). Bileşen AirLink'i panele kendi simgesi olarak ekler: tıklanınca AirLink'in tepsi paneli simgenin altında açılır (AirLink kapalıysa başlar). Simge tepsi simgesinin renklerini alır (başlatılıyor mavi, bağlı yeşil, cihaz yok turuncu, duraklatıldı ya da çalışmıyor gri, hata kırmızı); durumu `$XDG_RUNTIME_DIR/airlink.status` dosyasından okur (Flatpak'ta `$XDG_RUNTIME_DIR/app/com.plyg.AirLink/airlink.status`).

Bileşen, `install.sh`'e verilen programı çalıştırır (varsayılan `app/build/airlink`). Başka bir yol için onu verin; derlemeyi taşıdıktan sonra `install.sh`'i yeniden çalıştırın:

```bash
app/packaging/linux/install.sh /yol/airlink
```

Ardından panele sağ tıklayıp **Bileşen Ekle…**'yi seçin ve **AirLink**'i panele sürükleyin. Listede görünmüyorsa Plasma kabuğunu yeniden başlatın:

```bash
systemctl --user restart plasma-plasmashell
```

Sürümlerde bileşen AppImage, tar.gz ve Flatpak için `AirLink-<sürüm>.plasmoid` dosyası olarak gelir. Bu bileşen AirLink'i kendisi bulur: önce AirLink'in en son başlatıldığı dosya (AirLink kendi yolunu, AppImage'ın dosyası da dahil, `~/.local/state/airlink/launcher` dosyasına yazar), sonra `PATH`'teki `airlink`, sonra Flatpak. İlk tıklamadan önce AirLink'i bir kez başlatın. .deb bileşeni tüm kullanıcılar için kurar; onunla `.plasmoid` gerekmez.

Bileşeni başka bir bilgisayar için programın yolu sabit olarak paketlemek için (yol paketlerken yazılır):

```bash
widget=$(mktemp -d) && cp -r app/packaging/linux/plasmoid/. "$widget" && \
  sed -i "s|@AIRLINK_BIN@|/yol/airlink|" "$widget/contents/ui/main.qml" && \
  (cd "$widget" && python3 -m zipfile -c "$OLDPWD/AirLink.plasmoid" metadata.json contents) && rm -rf "$widget"
```

`kpackagetool6 --type Plasma/Applet --install AirLink.plasmoid` ile (yenisi için `--upgrade`) ya da **Bileşen Ekle… → Yeni Al → Yerel Dosyadan Bileşen Kur…** ile kurun. `install.sh --uninstall` ya da `kpackagetool6 --type Plasma/Applet --remove dev.airlink.panel` kaldırır.

### Windows

Linux'tan MinGW-w64 ile çapraz derlenir. Sonuç, yalnızca Windows'un kendi DLL'lerine bağlı tek bir `.exe`'dir.

```bash
rustup target add x86_64-pc-windows-gnu
```

```bash
cmake -S app -B app/build-windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=../kompoz/cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build app/build-windows
```

`airlink.exe` dosyasını `fonts/` ve `licenses/` klasörleriyle birlikte dağıtın. Vulkan olmayan sanal makineler ve Uzak Masaüstü oturumları için `python3 app/packaging/windows/package.py`, ANGLE'ın Direct3D 11 kitaplıklarını ve lisansını içeren `AirLink-Windows-ANGLE.zip` paketini oluşturur; bkz. [Windows deneme adımları](docs/windows-test.md). Çoklu yayın keşfi sanal ağı geçemiyorsa Cihazlar sekmesindeki **IP adresiyle eşleştir** alanına diğer cihazın IP adresini ve AirLink portunu girin. İlk açılışta, yalnızca o kullanıcı için, Explorer'ın sağ tık menüsüne ve "Gönder" menüsüne "AirLink ile gönder" ekler. Ayarlar'daki **Oturum açılınca başlat** anahtarı AirLink'i kullanıcının `Run` anahtarına `--background` ile ekler, böylece sistem tepsisinde başlar; paketli kurulum `airlink-pkg` kısayoluyla başlar. `airlink.exe --unregister` menüleri ve `Run` girdisini kaldırır.

### Android

Gerekenler:

- NDK 29.0.14206865
- SDK CMake 4.1.2
- `cargo-ndk`
- Rust hedefi `aarch64-linux-android`

```bash
cd app/android && ./gradlew installDebug
```

Android uygulama kimliği `com.plyg.airlink`. Eski `dev.airlink.airlink` kurulumu ayrı bir uygulamadır; yerel verisi yoksa cihazları yeniden eşleştirin.

İzinler telefonda, uygulamadaki ayrı kartlardan istenir:

- bildirim erişimi
- SMS ve kişiler
- telefon
- pil kısıtlaması olmadan çalışma (ekran kapalıyken de ulaşılabilir kalmak için)

AirLink telefon açıldığında ve güncellendiğinde arka planda başlar; bunu durdurmak için Ayarlar'da **Telefon açılınca başlat** anahtarını kapatın.

#### Gelişmiş mod

Telefon uygulamalarını bilgisayarda açmak, bir uygulamanın yapmasına izin verilenden fazlasını gerektirir: güvenilir sanal ekranlar oluşturmak ve girdi göndermek. AirLink bu yetkileri telefonun kendi **kablosuz hata ayıklama** özelliğinden alır. scrcpy gibi araçların yaptığı gibi, ama bilgisayar ya da kablo olmadan:

1. Telefonda *Geliştirici seçenekleri → Kablosuz hata ayıklama*'yı açın.
2. AirLink'te gelişmiş modu başlatın ve *Eşleme kodu ile cihaz eşle*'yi seçin.
3. Kodu AirLink'in bildirimine girin.

AirLink telefonun kendi ADB mDNS servisini bulamazsa, telefondaki *Kablosuz hata ayıklama* ekranında görünen bağlantı portunu **Gelişmiş mod → Kablosuz hata ayıklama portu** alanına girip **Dene**'ye dokunun. Yeni eşleştirmede bildirim, altı haneli kodun yanında görünen eşleme portuyla `123456:PORT` biçimini de kabul eder. Bağlantı ve eşleme portları farklıdır.

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

AirLink'in kaynak kodu [Sustainable Use License 1.0](LICENSE) ile erişime açıktır. Ticari yeniden dağıtım kısıtlandığı için OSI tanımına göre açık kaynak değildir:

- Kişisel, ticari olmayan veya kendi kuruluşunun iç işlerinde kullanılabilir ve değiştirilebilir.
- Orijinal ya da değiştirilmiş sürümler, lisans ve değişiklik bildirimleri korunarak ticari olmayan amaçlarla ücretsiz paylaşılabilir.
- Ticari yeniden dağıtım veya AirLink'i ücretli ürün ya da hizmet olarak başkalarına sunmak için Kasim'den ayrı bir yazılı lisans gerekir.
- Çatallar ve çekme istekleri memnuniyetle karşılanır; bkz. [CONTRIBUTING.md](CONTRIBUTING.md). Çatallar kendi adını ve görsel kimliğini kullanmalıdır; bkz. [TRADEMARKS.md](TRADEMARKS.md).

Üçüncü taraf bileşenler kendi lisanslarını korur (Kompoz: Apache 2.0, OpenH264 ve ANGLE: BSD, Rust paketleri: çoğunlukla MIT/Apache 2.0). Topluluk lisansının dışındaki izinler için [Kasim](https://github.com/kasimtmc) ile iletişime geçin.
