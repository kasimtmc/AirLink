# AirLink geliştirme durumu

## 24 Kasım 2026

Yayımlanmış başlangıç noktası `4fa0abc`. Bu oturumdaki değişiklikler çalışma ağacında duruyor. AirLink, Rust çekirdeğini ve Kompoz C++ arayüzünü kullanıyor. KompozR’a geçiş ayrı bir iş.

- Telefon uygulamaları Kompoz `Window` ile oturum başına bir masaüstü penceresinde açılıyor. Pencereler sekme değiştirildiğinde açık kalıyor. Uygulamalar sekmesindeki düğme açık pencereyi öne getiriyor. Pencerenin kapatma düğmesi mevcut uzak oturum kapatma akışını kullanıyor.
- Odaktaki uygulama görüntüsünde Kompoz’un UTF-8 metin olayları mevcut Android metin yoluna aktarılıyor. Gezinme tuşları ve klavye kısayolları Android tuş kodlarını kullanıyor. Klavye haritasının üretemediği metin mevcut telefon panosu üzerinden yapıştırılıyor.
- AirLink’in Android tarafındaki bütün yerel Java kaynakları Kotlin’e taşındı. Kompoz’un dış depodaki Android Java kaynakları Kompoz’a aittir.

### Doğrulama

- Linux derlemesi ve geçici kullanıcı diziniyle ekransız açılış geçti.
- `cargo test --workspace` ile 11 test geçti.
- Android `:app:compileDebugKotlin` ve `:app:compileDebugJavaWithJavac` geçti.
- Yerel Kompoz tabanı `15f95c8`. Kompoz’un pencere ve klavye API’leri için `docs/guide/en/setup.md` ve `include/kompoz/window.hpp` dosyalarına bakılabilir.

### Telefon bağlandıktan sonraki doğrulama

- `AirLinkAccessibility` Kotlin’e taşındı. Ekran paylaşımı etkin olduğunda hareketleri işleme koşulu korundu. Sınıfın manifest adı ve Java’nın kullandığı `enabled`, `openSettings` ve `input` statik imzaları doğrulandı.
- Android debug APK’sı derlendi ve Xiaomi 14 Ultra’ya USB üzerinden güncelleme olarak kuruldu. AirLink açıldı, ön plan servisi çalıştı ve Linux istemcisi telefonu yerel ağda keşfetti. Android erişilebilirlik servisini bağladı.
- Windows çapraz derlemesi geçti. WinBoat içindeki Windows denemesi aşağıda doğrulandı; fiziksel Windows bilgisayarda da kullanıcı doğruladı.
- Telefonun kablosuz hata ayıklaması yeniden açıldı. USB ve kablosuz ADB bağlantıları aynı anda görünüyor; `adb_wifi_enabled=1`. Linux AirLink açıldı. Kullanıcı iki uygulama penceresini, küçültüp geri getirmeyi, kapatmayı ve Türkçe yazı girişini gerçek cihazda doğruladı.
- `AdbPairing` ve bildirim yanıt alıcısı Kotlin'e taşındı. `AdbPairing$Receiver` manifest adı ile `start()` / `done(boolean, String)` statik JVM imzaları korundu. Android debug APK derlendi ve telefona yüklendi. AirLink ile ön plan ve erişilebilirlik servisleri yeniden başladı.
- Yardımcı sürece sanal ekrandaki görevi izleme eklendi. Görev kaybolduğunda `apps.event: closed` gönderiyor. Kullanıcı uygulamadan Geri ile çıkınca masaüstü penceresinin kapandığını doğruladı; günlükte görevin görünmesi ve kaybolması kaydedildi.
- Yardımcının `Helper` giriş noktası ve `Log` sınıfı Kotlin'e taşındı. Java çağrıları ve `app_process` için statik JVM metotları korundu. APK derlenip telefona kuruldu; `app_process / dev.airlink.airlink.helper.Helper` kullanımı başarıyla çalıştı. Kullanıcı bu sürümde uygulama görüntüsü ve dokunmayı yeniden doğruladı.
- Kullanıcı yatay uygulamada masaüstü görüntüsü ile dokunma koordinatlarının eşleştiğini doğruladı.
- Yardımcının `Workarounds` sınıfı Kotlin'e taşındı. Java'nın çağırdığı `apply()` ve `context()` statik JVM imzaları korundu; APK derlenip telefona kuruldu. `app_process` testinde sanal ekran oluşturma, Settings'i o ekranda açma, iki JPEG kare alma ve dokunma enjeksiyonu geçti. Test kareleri telefondan temizlendi.
- Kullanıcı hızlı Türkçe yazıda karakter sırası ile Ctrl+V yapıştırmanın doğru çalıştığını doğruladı.
- `AirLinkNotificationListener` Kotlin'e taşındı. JNI'nin çağırdığı `accessGranted()`, `openAccessSettings()`, `act(...)` ile `nativeBroadcast(...)` statik JVM imzaları doğrulandı. APK telefona kuruldu, sistem bildirim dinleyicisini bağladı ve bir bildirim olayı eşleşmiş cihaza yayınlandı (`broadcast notification.posted to 1 devices`).

### Tam Kotlin geçişi ve Android paket adı

- Kalan `AirLinkInCallService`, `AirLinkService`, `SmsBridge`, `CallBridge`, `ScreenShare`, `MainActivity`, `AppStreams`, `Input`, `AppDisplay` ve `Server` sınıfları Kotlin’e taşındı. `app/android/app/src/main/java` boşaldığı için dizin kaldırıldı.
- Android `namespace` ve `applicationId` artık `com.plyg.airlink`. Kotlin kaynakları `app/android/app/src/main/kotlin/com/plyg/airlink` altında. JNI sınıf aramaları ve dışa aktarılan fonksiyon adları yeni paketle eşleşiyor. Ekran paylaşım ve servis eylemleri ile `app_process` yardımcı komutu da güncellendi.
- `:app:assembleDebug` geçti; APK kimliği `aapt` ile ve statik JNI metotları `javap` ile doğrulandı. Yeni APK Xiaomi 14 Ultra’da çalışıyor. Ön plan, bildirim ve erişilebilirlik servisleri bağlı. Yeni paketle `app_process` sanal ekran oluşturdu, Settings’i başlattı, iki JPEG kare aldı ve dokunma enjekte etti; test kareleri temizlendi.
- Eski `dev.airlink.airlink` uygulaması ve özel veri dizini telefonda bulunmadı. Bu nedenle eski eşleşme anahtarları taşınamadı. Kullanıcı yeni uygulamayla yeniden eşleştirdi; telefon ve masaüstündeki güven kayıtlarının karşılıklı kimlikleri eşleştirdiği doğrulandı. Eski telefon kimliği geri getirilmiş olarak gösterilmemeli.
- Kullanıcı yeni paketle bilgisayardan telefona dosya aktarımını, bilgisayarda telefon uygulaması açmayı ve dokunmayı doğruladı. Yeni paketin SMS, kişiler, arama ve bildirim izinleri kullanıcı 0 için verilmiş; bildirim dinleyicisi bağlı. USB ADB ayrıldıktan sonra kablosuz ADB bağlantısı çalışmaya devam etti.

### Devam sırası

Telefon bölümü: masaüstündeki "Aramalar" ana sekmesi "Telefon" oldu; içinde "Aramalar" ve "Rehber" alt sekmeleri var. Rehber Android `READ_CONTACTS` izniyle ad, numara ve etiketleri `contacts.list`/`contacts.page` üzerinden 100 kayıtlık sayfalarda getiriyor; arama ve kişiden arama eklendi. Kompoz `LazyListState.layoutInfo` ile liste sonuna yaklaşınca sonraki sayfa otomatik yükleniyor. Arama geçmişindeki 60 kayıt sınırı kaldırıldı; `calls.log_request`/`calls.log` artık 80 kayıtlık sayfalarla bütün geçmişi yükleyebiliyor. Linux ve Android debug derlemeleri geçti, yeni APK telefona kuruldu ve masaüstü uygulaması açıldı. Kullanıcı rehberin ve arama geçmişinin otomatik yüklenmesini, aramayı ve kişiden aramayı doğruladı.

1. Yeni pakette bildirim ve yanıt ile SMS gönderme/alma kullanıcı tarafından doğrulandı. Arama durumları bilgisayara ulaşıyor. İlk denemede ahize/hoparlör geçişi çalışmıyor ve yeni aramada önceki kişinin adı kalıyordu. Kotlin arama köprüsü Android 12+ için `setCommunicationDevice` kullanıyor ve yeni/bitmiş aramada önceki numarayı temizliyor. Kullanıcı gerçek aramada ses çıkışı ve seçili seçeneğin güncellendiğini, kayıtlı olmayan numaranın doğru gösterildiğini doğruladı. Arama geçmişi artık eski `CACHED_NAME` yerine güncel rehber adını çözüyor; son APK derlenip telefona kuruldu ve kullanıcı bilinmeyen test numarasının geçmişte doğru göründüğünü doğruladı.
2. JPEG akışından H.264'e geçiş uygulandı: Android `MediaCodec` esnek YUV girişli kodlayıcı, bağımsız `ALH1` anahtar kareleri ve hata halinde JPEG dönüşü; Rust C ABI içinde OpenH264 çözücü ve masaüstünde Kompoz RGBA görüntüye dönüşümü. Masaüstü `screen.start`/`apps.open` istekleri H.264 istiyor; eski Android sürümlerinin JPEG kareleri okunmaya devam ediyor. Rust çözücü testi ile Linux, Windows ve Android derlemeleri geçti. OpenH264 lisansı Android varlığına ve Windows paketine eklendi; Windows ZIP bütünlüğü geçti ve WinBoat paylaşımına kopyalandı. Android 34 x86_64 emülatöründeki kabuk yardımcısı Settings'i sanal ekranda açtı ve iki bağımsız H.264 kare üretti; ilk kare FFprobe ile 720×1280 olarak açıldı, aynı kare Rust C ABI çözücüsüyle 720×1280 RGBA'ya dönüştürüldü. Emülatördeki geçici kareler temizlendi ve emülatör kapatıldı. Fiziksel Xiaomi 14 Ultra'ya arm64 APK kuruldu; kabuk yardımcısı Settings'i sanal ekranda başlatıp iki bağımsız H.264 kare üretti. İlk gerçek telefon karesi Rust C ABI çözücüsüyle 720×1280 RGBA'ya çevrildi. Kapanışta kare işleme sürerken kodlayıcının serbest bırakılması bir yarış hatası oluşturuyordu; `AppDisplay.release()` temizliği aynı iş parçacığına sıralandı ve yeniden yapılan gerçek telefon testinde hata tekrarlanmadı. Kullanıcı canlı H.264 uygulama penceresini ve ekran paylaşımını gerçek telefonda doğruladı.
   Kullanıcı Gelişmiş modda `wireless debugging is off: not found: _adb-tls-connect._tcp.local.` hatası gördü. USB ADB ile `adb_wifi_enabled=1` ve shell uid 2000'e ait dinleyen TLS portu doğrulandı; sorun Android'in kendi mDNS kaydının uygulama tarafından bulunamaması. Rust `shell_at`/C ABI `airlink_adb_shell_port` ve Gelişmiş modda isteğe bağlı bağlantı portu alanı eklendi. Geçerli port uygulamanın özel veri dizinindeki `adb/connect-port` dosyasına kaydediliyor; boş alan mDNS yolunu kullanıyor. Mevcut bağlantı portu 46523 dosyaya yazıldı, yeni APK telefona kuruldu. Kullanıcı “Dene” ile kabuk yetkisinin çalıştığını doğruladı. İlk eşleşmede mDNS bulunamazsa bildirimde `kod:eşleme_portu` girmeyi sağlayan `pair_at`/`airlink_adb_pair_port` yolu da eklendi; kullanıcı bu yolu telefonda denedi ve çalıştığını doğruladı.
3. Windows çapraz derlemesi ve WinBoat üzerinde temel çalışma doğrulandı; deneme adımları `docs/windows-test.md` içinde. Kullanıcı fiziksel Windows bilgisayarda denemeyi yaptı ve çalıştığını doğruladı.
   WinBoat'ta uygulamaya tıklayınca yalnızca meşgul imleç göründü; pencere açılmadı. Windows PE bağımlılıkları yalnızca sistem DLL'leri. Aynı `airlink.exe` Wine'da açılıp Kompoz Vulkan aygıtını ve yazı tiplerini başlattı; bu sonuç WinBoat sanal GPU yolunu doğrulamıyor. Windows'a özel `AIRLINK_DIAG_LOG` başlangıç günlüğü ve `diagnose.cmd` eklendi; tanılama paketi derlenip bütünlük kontrolünden geçti.
   WinBoat günlüğü: `no window for vulkan: no Vulkan loader`, ardından `std::length_error` (`basic_string::_M_create`). Kompoz C++ `platform/common/egl_window.cpp` içinde `eglLibrary().begin()` ve ayrı bir geçici nesnenin `.end()` değeri birlikte kullanılıyordu; kardeş Kompoz deposunda tek bir yerel `std::string` üzerinden alınacak şekilde düzeltildi. Mevcut Codex görev listesinde Kompoz C++ oturumu bulunamadığı için düzeltme doğrudan kardeş depoda yapıldı; o depodaki başka değişikliklere dokunulmadı. Windows çapraz derlemesi geçti. EdgeFirstAI `angle-package` `v2.1.28252` Windows x64 arşivi SHA-256 ile doğrulandı ve ANGLE DLL'leri/lisansı `AirLink-Windows-ANGLE.zip` paketine eklendi. Wine'da `KOMPOZ_GPU=gles` ile ANGLE Direct3D 11 aygıtı ve yazı tipleri başlatıldı. ANGLE olmadan zorlanmış GLES yolunda da artık `length_error` oluşmuyor, grafik aygıtı yokluğu günlüğe düzgün yazılıyor. Kullanıcı WinBoat'ta yeni paketin penceresinin açıldığını ve sekmelerin çalıştığını doğruladı; günlüğü `Microsoft Basic Render Driver` üzerinde ANGLE Direct3D 11 aygıtı ve yazı tiplerini gösteriyor. Ağ ve Explorer denemeleri sürüyor. Paketleme betiği `app/packaging/windows/package.py`, deneme adımları `docs/windows-test.md` içinde. Paket ayrıca `/home/kasim/AirLink-Windows-ANGLE.zip` yoluna kopyalandı (WinBoat paylaşımı).
   WinBoat'ta Explorer sağ tık menüsü ve seçilen dosyayı AirLink'e aktarma çalışıyor; mDNS ile cihazlar görünmüyor. Wine'da ANGLE yolu açılıyor fakat Wine'ın D3D shader derlemesi siyah pencereye yol açıyor; Rust ağ çekirdeği `OS Error 10045` ile başlayamıyor. C ABI'ye `airlink_start_pairing_address` (IP veya IP:port), arayüze "IP adresiyle eşleştir" eklendi. Eşleşmeyi kabul eden tarafın bağlantıyı güvenilir eşleşme sonrası önbelleğe alması sağlandı; mDNS kapalı iki düğümlü Rust testinde bağlantı erişilebilir olarak doğrulandı. Rust testleri ile Linux, Windows ve Android derlemeleri geçti. ANGLE paketi yenilenip WinBoat paylaşımına kopyalandı. Kullanıcı WinBoat'tan Linux AirLink'e `192.168.0.182:47820` üzerinden bağlandı; eşleştirme kodu iki tarafta göründü, iki tarafta onaylandı ve küçük dosya aktarımı WinBoat → Linux ile Linux → WinBoat yönlerinde çalıştı. Kullanıcı WinBoat'ta çoklu dosya aktarımını ve sürükle bırakmayı da doğruladı. Fiziksel Windows bilgisayarda yerel ağ keşfi dahil deneme kullanıcı tarafından doğrulandı; WinBoat ve Wine'daki sorunlar sanal ortama özgü.
4. Bilgisayarı telefondan kontrol, el hareketiyle aktarım ve Bluetooth HFP arama sesi sonraki fazda. Lisans seçimi ve Android etkinliğinin servis açıkken yeniden oluşturulması da açık.

## Yol haritası karşılaştırması (24 Kasım 2026)

Kod, commit geçmişi (`7581971` → `4fa0abc`) ve çalışma ağacı README'deki faz tablosuyla karşılaştırıldı.

| Faz | Planlanan | Kodda olan | Doğrulama | Eksik |
|---|---|---|---|---|
| 1 | Keşif, eşleştirme, QUIC/TLS, dosya, pano | mDNS keşfi, SAS eşleştirme, dosya/klasör, pano; ek olarak IP ile eşleştirme | `cargo test` (e2e loopback), Linux↔Android, WinBoat↔Linux, fiziksel Windows | — |
| 2 | Bildirimler, SMS, rehber, aramalar | `notification.*`, `sms.*`, `contacts.search/list`, `calls.log`, `call.*` (yanıtla, reddet, kapat, beklet, sessiz, ses yolu, ses düzeyi, arama başlatma) | Xiaomi 14 Ultra'da kullanıcı doğruladı | Bluetooth ile arama sesinin bilgisayara gelmesi (Faz 4) |
| 3 | Telefon ekranı ve uygulamaları bilgisayarda | `screen.*` (erişilebilirlikle dokunma), `apps.*` (kabuk yardımcısı, sanal ekran, ayrı pencere, klavye/teker), H.264 + JPEG dönüşü | Pencereler, girdi, yatay mod, canlı H.264 uygulama penceresi, ekran paylaşımı ve `kod:port` ile ADB eşleşmesi gerçek cihazda doğrulandı | — |
| 4 | PC'yi telefondan kontrol, el hareketiyle aktarım, Bluetooth arama sesi | Başlanmadı; `input.*` ve `gesture.*` yalnızca ayrılmış | — | Tamamı |

Planda olmayan ama yapılan işler: Flutter yerine Kompoz C++ arayüzü, Android uygulamasının Java'dan Kotlin'e taşınması ve `com.plyg.airlink` paket adı, Windows ANGLE paketi ve tanılama betiği, Kompoz `egl_window.cpp` düzeltmesi (kardeş depoda).

Bu karşılaştırmada güncellenen belgeler: `docs/protocol.md` (eksik `call.*`, `screen.*`, `apps.*`, `calls.error` mesajları eklendi, `sms.send` alanları düzeltildi, eskimiş "gelecek sürümler" listesi Faz 4'e indirildi, H.264 doğrulama notu güncellendi) ve `README.md` / `README.tr.md` (Windows durumu, H.264, Türkçe README'nin İngilizceyle eşitlenmesi: ANGLE paketi, IP ile eşleştirme, paket adı, ADB port alanı).

Kullanıcı canlı H.264 penceresi ve ekran paylaşımını, `kod:port` ile ilk ADB eşleşmesini ve fiziksel Windows denemesini doğruladı. Faz 1–3 tamam.

Açık işler: (1) çalışma ağacının commit'lenmesi (kullanıcı isteyince), (2) lisans seçimi, (3) servis açıkken Android etkinliğinin yeniden oluşturulması, (4) Faz 4.

Kompoz’un Android Java kaynakları AirLink deposunun dışında, Kompoz deposunda tutuluyor.

Commit ve push kullanıcı istediğinde yapılır. Commit yazarı yalnızca Kasim olur. Commit mesajlarına ortak yazar veya model atfı eklenmez.
