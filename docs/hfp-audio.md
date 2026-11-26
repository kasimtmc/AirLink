# Telefon görüşmesinin bilgisayar sesi (Faz 4C)

Android, üçüncü taraf uygulamalara normal telefon görüşmesinin giden/gelen sesini MediaProjection ile vermez. AirLink bu sesi telefonun Bluetooth Hands-Free Profile (HFP) Audio Gateway rolünden alır; Linux bilgisayar Hands-Free rolündedir. [Android ses yakalama kuralları](https://developer.android.com/media/platform/sharing-audio-input), [WirePlumber Bluetooth rolleri](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html).

## Linux uygulaması

- Gerekenler: BlueZ, PipeWire, WirePlumber, `bluetoothctl` ve `pactl`. Telefon bilgisayarla Bluetooth üzerinden eşleşmiş olmalı. WirePlumber'ın `hfp_hf` rolü varsayılan yapılandırmada açıktır; dağıtım farklı yapılandırılmışsa bu rol etkinleştirilmelidir.
- Görüşme sırasında **Bilgisayardan konuş** düğmesi eşleşmiş telefonu Bluetooth üzerinden bağlar. Android'den `call.route=bluetooth` ister.
- HFP ses uçları açılınca telefonun konuşma çıkışı bilgisayarın varsayılan hoparlörüne, bilgisayarın varsayılan mikrofonu telefonun konuşma girişine bağlanır. Bağlantılar PipeWire PulseAudio uyumlu `module-loopback` modülleriyle kurulur; ses AirLink QUIC ağına veya dosyaya yazılmaz. [PipeWire loopback](https://pipewire.pages.freedesktop.org/pipewire/page_pulse_module_loopback.html).
- Düğmeyle kapatınca veya görüşme bitince modüller kaldırılır. Varsayılan çıkış ve giriş görüşmeye başlamadan önce seçilmelidir; görüşme sırasında değiştirilirse köprü yeniden başlatılır.
- Ses uçları görüşme dışında görünmeyebilir. Eşleşme ve bağlantı `bluetoothctl info <telefon-adresi>`, ses uçları `pactl list sinks short` / `pactl list sources short` ile incelenebilir.

## Doğrulama durumu

Linux makinede Xiaomi 14 Ultra ile Bluetooth eşleşmesi ve bağlantısı görüldü. Telefonun HFP Audio Gateway hizmeti ilan ediliyor. Gerçek görüşmede gelen sesi bilgisayarda duydum; bilgisayar mikrofonunun karşı tarafa ulaştığını, düğmeyle sesi telefona geri alabildiğimi ve görüşme bitince arka planda ses akışı kalmadığını doğruladım. Bekletme/sürdürme ayrıca doğrulanmadı.

Bu görüşme denemesi önceki masaüstü düğmesiyle yapıldı. Sonraki sürümde aynı HFP köprüsünü
telefondaki anahtar başlatıyor; bu yeni başlatma yolu gerçek görüşmede ayrıca denenecek.

PipeWire'ın `module-loopback` yükleme/kaldırması sessiz bir geçici uçla doğrulandı. Android APK, Linux uygulaması ve Windows çapraz derlemesi başarılıdır.

## Windows uygulaması

Windows tarafı Rust çekirdeğinde (`core/airlink-desktop/src/bt_audio_windows.rs`), WinRT üzerinden çalışır. C++ katmanı `app/src/call_audio_windows.cpp`, C ABI'si `airlink_win_*` adıyla tanımlıdır.

- **Arama sesi (HFP Hands-Free):** [`PhoneLineTransportDevice`](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.calls.phonelinetransportdevice) ile eşleşmiş telefon bulunur. Sonra `RequestAccessAsync`, `RegisterApp` ve `ConnectAsync` ile HFP hizmet bağlantısı kurulur. Linux'taki gibi bir döngü kurulmaz: Windows, `AudioRoutingStatus` değeri `CanRouteToLocalDevice` olduğunda SCO sesini kendisi yerel hoparlöre ve mikrofona verir (Phone Link'in kullandığı yol). Bu özellik bulunmayan eski sürümlerde, bağlantıdan 3 saniye sonra ses etkin sayılır. 40 saniye içinde ses yolu açılmazsa hata gösterilir. Kapatınca `UnregisterApp` çağrılır; böylece sonraki aramalar telefonda kalır.
- API kısıtlı `phoneLineTransportManagement` yetkisini ister ve paket kimliği gerektirir. Bunun için Geliştirici Modu'nda klasörden kaydedilen bir paket kullanılır (`app/packaging/windows/appx/AppxManifest.xml`, `install-package.cmd`). Mağaza onayı gerekmez. Manifest AppData ve kayıt defteri sanallaştırmasını kapatır, böylece eşleşmeler paketsiz sürümle ortak kalır.
- **Oynatıcı sesi (A2DP alıcısı):** [`AudioPlaybackConnection`](https://learn.microsoft.com/en-us/uwp/api/windows.media.audio.audioplaybackconnection) (Windows 10 2004+) seçili oynatıcı çaldığı sürece açık tutulur ve Windows varsayılan çıkıştan çalar. Paket kimliği gerekmez.
- Bilinen riskler: Açık kaynak [MyPhone](https://github.com/BestOwl/MyPhone) projesi aynı API ile hizmet bağlantısı kurmuş ama SCO sesini aktaramamış. Bu proje sesi kendi sürücüsüyle almaya çalışıyordu; AirLink ise Windows'un yerel yönlendirmesine güveniyor. Bazı Windows 22H2 kurulumlarında `FromId` çağrısının boş döndüğü bildirilmiş. Phone Link aynı telefonda aramalar için açıksa bağlantıyı o tutabilir.
- Doğrulama: Windows çapraz derlemesi geçti. Fiziksel Windows bilgisayarda henüz denenmedi. Deneme adımları `docs/windows-test.md` içinde.

## Diğer ses akışları

“Oynatıcı sesini aktar” seçili uygulama oynatırken eşleşmiş Linux bilgisayara Bluetooth
A2DP profili üzerinden telefonun medya çıkışını verir. Android MediaSession durumu mevcut
bildirim erişimiyle okunur. “Ekran paylaşımı sesi” ise ekran oturumunda QUIC üzerinden
PCM taşır. Bu üç akışın kontrolleri ayrıdır.
