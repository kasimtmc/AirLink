# AirLink geliştirme durumu

## 24 Kasım 2026

Bu dönemin başlangıç noktası `4fa0abc`. AirLink, Rust çekirdeğini ve Kompoz C++ arayüzünü kullanıyor. KompozR’a geçiş ayrı bir iş.

## 25 Kasım 2026: lisans ve katkı koşulları

- PolyForm Strict 1.0.0 yerine Sustainable Use License 1.0 seçildi. Kod kaynak erişimli olarak tanımlanır: kişisel, ticari olmayan ve kuruluş içi kullanım ile değişiklik yapılabilir; değiştirilmiş sürümler de ticari olmayan amaçlarla ücretsiz paylaşılabilir. Ticari yeniden dağıtım için Kasim'den ayrı yazılı lisans gerekir. Bu kısıtlama nedeniyle OSI anlamında açık kaynak denmez.
- `README.md`, `README.tr.md`, Cargo paket metadata'sı, katkı koşulları ve marka açıklaması yeni lisansla eşitlendi. Katkıcı telifini korur; projeye verilen geniş lisans, topluluk ve ayrı ticari lisansların sürdürülebilmesi içindir. Çatallar kendi adı ve görsel kimliğiyle dağıtılabilir. Üçüncü taraf bileşenler kendi lisanslarında kalır.

## 25 Kasım 2026: masaüstü tepsisi ve Android arka planı

- AirLink tepsi menüsüne cihaz başına Dosya gönder, Panoyu gönder ve Android için Ekranı göster eklendi. Cihaz alt menüsü yalnızca eşleşmiş cihaz çevrimiçi olduğunda etkin. Ayrıca Gelen dosyaları aç ve Paylaşımı duraklat/sürdür var. Duraklatma Rust düğümünü gerçekten kapatıyor; açık aktarım ve istekleri duraklatılmış olarak temizliyor. KDE oturumunda menü yapısı DBus üzerinden doğrulandı; duraklatınca UDP 47820 kapandı, sürdürünce aynı süreçte tekrar açıldı ve telefon yeniden çevrimiçi göründü. Açılış hatası sonrası menüden bağlantıyı yeniden deneme yolu var.
- Tepsi simgesi başlatılırken mavi, bağlı cihaz varken yeşil, bağlı cihaz yokken turuncu, paylaşım duraklatılınca gri ve hata durumunda kırmızı. İşlem/aktarım hatasının kırmızısı 30 saniye sonra temizleniyor. KDE StatusNotifierItem verisinde yeşil, turuncu, gri ve kırmızı simge pikselleri ile ilgili ipuçları doğrulandı. Linux, Windows ve Android derlemeleri geçti; kullanıcı WinBoat tepsi denemesini tamamladı.
- Kullanıcı, cihaz alt menüsünün KDE'de hâlâ gri göründüğünü bildirdi. Kompoz'un Linux DBusMenu sunucusu, bir öğe yeniden etkinleştiğinde `enabled` özelliğini çıkarmak yerine açıkça `enabled=true` gönderiyor. Gerçek AirLink tepsisinde duraklat → `enabled=false`, sürdür → `enabled=true` geçişi doğrulandı. Kullanıcı Xiaomi'nin bağlıyken etkin olduğunu doğruladı; çevrimdışı eşleşmiş cihaz da etkin göründüğü için AirLink menüsü tekrar `peer.online` koşuluna bağlandı. Yeniden derlenip açılan KDE tepsisinde çevrimdışı KASIMWI için `enabled=false`, bağlı Xiaomi için `enabled=true` okundu; Linux ve Windows derlemeleri geçti.
- Linux/Wayland'da tepsiden her geri açışta eski pencerenin ekranda kalması düzeltildi. Kompoz'un `setWindowShown(false)` yolu `wl_proxy_destroy` ile yalnızca istemci tutamağını bırakıyordu; artık frame callback, dekorasyon, `xdg_toplevel` ve `xdg_surface` için gerçek protokol yok etme çağrıları yapılıyor. KDE oturumunda ayrı bir Kompoz test uygulaması DBus tepsi menüsüyle beş kez gizlenip açıldı: KWin her turda `removed 0`, `added 1` pencere kaydetti. Eski AirLink tepsi menüsünden kapatıldı, düzeltmeli Linux derlemesi açıldı.
- Kompoz deposunda herkese açık `Tray`, `setAppWindowVisible`, `isTraySupported` ve `exitApplication` API'leri eklendi. Linux StatusNotifierItem/DBusMenu, Windows bildirim alanı uygulamaları var. AirLink masaüstünde tepsi simgesini, bağlı cihazları, açma ve çıkış menüsünü kullanıyor; pencere kapatılınca tepsi destekleniyorsa gizleniyor.
- Linux tepsi etkinleştirmesi ve menü tıklamasında DBus olay kilidinin ikinci kez alınmasıyla oluşan kilitlenme giderildi. KDE oturumunda simge kaydı, DBus `Activate`, menüden Aç ve Çık gerçek AirLink sürecinde denendi; süreç Çık ile temiz biçimde sonlandı.
- Android `AirLinkService` ön plan servisi süreç yeniden başlatıldığında çekirdeği Activity açılmadan kuruyor ve `START_STICKY` ile geri geliyor. Activity yok edildiğinde JNI Activity başvurusu temizleniyor; açık dosya seçimi iptal ediliyor. Kullanıcının Durdur eylemi servisi durdurmaya devam ediyor.
- Kompoz birim testleri: 763 test, 0 başarısız. AirLink Linux ve Windows derlemeleri geçti. Android x86_64 APK, Android 14 test emülatörüne kuruldu: ana ekran üzerinden arka plana alındı; yalnızca emülatörde uygulama süreci sonlandırıldı. Servis yeni PID ile ön planda yeniden başladı, aynı veri dizininde Rust kimliği kaldı ve UDP 47820 dinlemeye döndü. Activity yeniden açıldı, yeni süreç yerinde kaldı. arm64 debug APK yeniden derlendi.
- Xiaomi 14 Ultra'ya güncel arm64 APK USB üzerinden kuruldu. AirLink açıldı, ön plan servisi başladı. Ana ekrana geçince süreç PID 19780 olarak kaldı; servis hâlâ foreground ve UDP 47820 dinliyor. AndroidRuntime hata günlüğünde çökme yok. Kullanıcı Android bildirimine dokununca uygulama ekranının geldiğini ve bağlantının sürdüğünü doğruladı. Telefonu iterek gönderme hareketi gerçek cihazda çalıştı.
- Gerçek telefonun uygulama süreci arka planda `run-as ... kill -9` ile ilk kez sonlandırıldığında Xiaomi `AutoStartManagerService` bildirim dinleyicisinin yeniden bağlanmasını reddetti; AirLink süreci ve ön plan servisi kendiliğinden geri gelmedi. Uygulama açılınca servis ve bağlantı geri geldi. Xiaomi cihazlarda Cihazlar ekranına “Arka planda bağlı kal” kartı eklendi; otomatik başlatma ayarına doğrudan, bu yol yoksa uygulama bilgisi ayarına götürüyor. Kullanıcı bu izni açtı. İkinci gerçek telefon denemesinde süreç 9057'den 10787'ye kendiliğinden yenilendi, ön plan servisi ve UDP 47820 geri geldi, Linux tepsisinde Xiaomi çevrimiçi göründü. Güncel arm64 APK kuruldu; Linux ve Windows derlemeleri geçti. Windows paketi yeni exe ve lisansla yenilenip bütünlüğü doğrulandı.
- WinBoat paylaşımındaki `/home/kasim/AirLink-Windows-ANGLE.zip`, son tepsi düzeltmelerini içeren Windows derlemesiyle yenilendi. ZIP bütünlüğü, içindeki `airlink.exe` ile yerel derlemenin eşitliği ve paylaşılan kopyanın eşitliği doğrulandı. Kullanıcı WinBoat tepsi denemesinin de tamamlandığını bildirdi.

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
4. Bilgisayarı telefondan kontrol, el hareketiyle aktarım ve Bluetooth HFP arama sesi sonraki fazda. Android etkinliğinin servis açıkken yeniden oluşturulması da açık.

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

Çalışma ağacı `ffdbb52` ile commit'lendi. O tarihte PolyForm Strict 1.0.0 seçilmişti; 25 Kasım 2026 tarihli lisans değişikliği yukarıda kayıtlı.

Açık işler: (1) Faz 4B+ ve 4C, (2) Faz 5.

## Faz 4 ve Faz 5 planı (24 Kasım 2026)

Sıra kullanıcı kararıyla: A → B → C, ardından son faz olarak arayüz iyileştirmesi.

| İş | Kapsam | Durum |
|---|---|---|
| 4A Bilgisayarı telefondan kontrol | Bilgisayar ekranı telefonda (H.264), dokunma fare, iki parmak kaydırma, tuşlar ve yazı; bilgisayarda onay ve "Durdur" çubuğu | Tamam; kullanıcı gerçek cihazda doğruladı |
| 4B El hareketiyle aktarım | Telefonu bilgisayara doğru itme ("fırlatarak gönder") | Gerçek cihazda kullanıcı tarafından doğrulandı |
| 4B+ Havada tut-bırak (Huawei tarzı) | Kamerayla yumruk (tut) ve açık avuç (bırak) tanıma; telefon ⇄ bilgisayar | Pano ve dosya iki yönde gerçek cihazda doğrulandı |
| 4C Bluetooth ile arama sesi | Bilgisayar telefonun kulaklığı gibi (HFP) | Başlanmadı |
| 5 Arayüz iyileştirmeleri | Görsel düzen, gezinme ve kullanılabilirlik | Başlanmadı |

4A ayrıntıları:

- Yeni Rust paketi `core/airlink-desktop`: Linux'ta xdg-desktop-portal RemoteDesktop oturumu (klavye + işaretçi) ve aynı oturumda ScreenCast akışı, PipeWire ile kare okuma; masaüstünün izni `desktop-share.token` ile saklanır, sonraki seferlerde masaüstü sormaz. Windows'ta birincil ekran GDI ile (imleç çizilerek) yakalanır, giriş `SendInput` ile oynatılır. Kareler OpenH264 ile bağımsız `ALH1` anahtar karelerine kodlanır (telefon ekranıyla aynı biçim), en çok 1600 px genişlik, 15 kare/sn.
- C ABI: `airlink_desktop_supported/start/stop/input`, olaylar `desktop_started`, `desktop_stopped`. Android'de bu paket derlenmez.
- Protokol: `desktop.start/stop/started/stopped/input`, medya akışı `desktop` (`docs/protocol.md`).
- Arayüz: telefondaki eşleşmiş bilgisayar kartında "Bilgisayarı kontrol et" düğmesi; tam ekran görünümde dokunma sol tık, çift dokunma çift tık, uzun basma sağ tık, sürükleme sol tuş basılı sürükleme, iki parmak kaydırma; Ctrl/Alt/Shift/Super seçicileri, sık tuşlar ve yazı alanı. Bilgisayarda izin penceresi ve paylaşım sürerken kırmızı "Durdur" çubuğu.
- Doğrulama: `airlink-desktop` birim testleri (kodlanan her karenin tek başına çözülmesi, boyut, giriş ayrıştırma, keysym eşlemesi), Linux ve Windows çapraz `clippy` temiz. Kullanıcı gerçek masaüstünde ekran yakalamayı ve telefondan fare/klavye kontrolünü doğruladı.

Kompoz’un Android Java kaynakları AirLink deposunun dışında, Kompoz deposunda tutuluyor.

4B+ uygulaması: Kamera yalnızca kullanıcı “Kamerayı aç” dediğinde çalışır ve çalışma sırasında izin ister. MediaPipe Gesture Recognizer modelinde `Closed_Fist` en az 300 ms ve %75 güvenle görülürse tutulur. Gerçek Xiaomi denemesinde `Open_Palm` puanları %53–70, karelerin çoğu `None` olduğundan, bırakma için sınıflandırmaya ek olarak el eklemlerinden dört parmağın en az üçünün açık olması ölçülür; 1 saniye içinde en az iki açık avuç gözlemi aranır. Yumruk 12 saniye sonra geçersizleşir, gönderimden sonra 3 saniye beklenir. Kareler telefon üzerinde işlenir, kaydedilmez veya ağa gönderilmez. Telefondan bilgisayara hazır dosya/pano gönderilir; Android paylaşım penceresinde kamerayı açan ayrı düğme vardır. Ters yönde bilgisayarda belli bir eşleşmiş telefon için dosya ya da pano 60 saniyeliğine hazırlanır; telefon hareketi tek kullanımlık `gesture.pull` isteğiyle bunu alır. Modelin Apache-2.0 bildirimi APK içindeki `assets/licenses/MediaPipeGestureRecognizer.txt` dosyasındadır. Pano ve dosya, her iki yönde gerçek Xiaomi ve Linux cihazlarında doğrulandı.

Commit ve push kullanıcı istediğinde yapılır. Commit yazarı yalnızca Kasim olur. Commit mesajlarına ortak yazar veya model atfı eklenmez.

4B ayrıntıları:

- `FlingDetector.kt`: doğrusal ivme sensörü (yerçekimi çıkarılmış). Telefon kullanıcıya dönükken ekranın dik ekseninde (cihaz -Z) 11 m/s² üstü itme, 450 ms içinde ters yönde 7 m/s² üstü durma; yanal ivme Z tepesinin %60'ından az olmalı. Tetiklemeler arası 2 sn. Yalnızca AirLink ekrandayken dinler (Android panoyu yalnızca ön plandaki uygulamaya verir; pil). Titreşimle onay.
- Hedef: son dosya/pano gönderilen ya da kontrol edilen bilgisayar (`fling-target`, veri klasöründe); çevrimiçi değilse tek çevrimiçi bilgisayar. Paylaşım menüsünden gelip bekleyen dosyalar varsa onlar, yoksa panodaki metin gider. Yeni protokol mesajı yok: mevcut dosya ve pano aktarımı kullanılır.
- Arayüz: telefonda "Fırlatarak gönder" kartı (aç/kapat, hedef), paylaşım penceresinde fırlatma ipucu.
- Eşikler sabit; gerçek cihazda yanlış tetikleme ya da kaçırma olursa ayarlanacak.
