# Telefon görüşmesinin bilgisayar sesi (Faz 4C)

Android, üçüncü taraf uygulamalara normal telefon görüşmesinin giden/gelen sesini MediaProjection ile vermez. AirLink bu sesi telefonun Bluetooth Hands-Free Profile (HFP) Audio Gateway rolünden alır; Linux bilgisayar Hands-Free rolündedir. [Android ses yakalama kuralları](https://developer.android.com/media/platform/sharing-audio-input), [WirePlumber Bluetooth rolleri](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html).

## Linux uygulaması

- Gerekenler: BlueZ, PipeWire, WirePlumber, `bluetoothctl` ve `pactl`. Telefon bilgisayarla Bluetooth üzerinden eşleşmiş olmalı. WirePlumber'ın `hfp_hf` rolü varsayılan yapılandırmada açıktır; dağıtım farklı yapılandırılmışsa bu rol etkinleştirilmelidir.
- Görüşme sırasında **Bilgisayardan konuş** düğmesi eşleşmiş telefonu Bluetooth üzerinden bağlar. Android'den `call.route=bluetooth` ister.
- HFP ses uçları açılınca telefonun konuşma çıkışı bilgisayarın varsayılan hoparlörüne, bilgisayarın varsayılan mikrofonu telefonun konuşma girişine bağlanır. Bağlantılar PipeWire PulseAudio uyumlu `module-loopback` modülleriyle kurulur; ses AirLink QUIC ağına veya dosyaya yazılmaz. [PipeWire loopback](https://pipewire.pages.freedesktop.org/pipewire/page_pulse_module_loopback.html).
- Düğmeyle kapatınca veya görüşme bitince modüller kaldırılır. Varsayılan çıkış ve giriş görüşmeye başlamadan önce seçilmelidir; görüşme sırasında değiştirilirse köprü yeniden başlatılır.
- Ses uçları görüşme dışında görünmeyebilir. Eşleşme ve bağlantı `bluetoothctl info <telefon-adresi>`, ses uçları `pactl list sinks short` / `pactl list sources short` ile incelenebilir.

## Doğrulama durumu

Linux makinede Xiaomi 14 Ultra ile Bluetooth eşleşmesi ve bağlantısı görüldü. Telefonun HFP Audio Gateway hizmeti ilan ediliyor. Gerçek görüşmede kullanıcı gelen sesi bilgisayarda duyduğunu, bilgisayar mikrofonunun karşı tarafa ulaştığını, düğmeyle sesi telefona geri alabildiğini ve görüşme bitince arka planda ses akışı kalmadığını doğruladı. Bekletme/sürdürme ayrıca doğrulanmadı.

Bu görüşme denemesi önceki masaüstü düğmesiyle yapıldı. Sonraki sürümde aynı HFP köprüsünü
telefondaki anahtar başlatıyor; bu yeni başlatma yolu gerçek görüşmede ayrıca denenecek.

PipeWire'ın `module-loopback` yükleme/kaldırması sessiz bir geçici uçla doğrulandı. Android APK, Linux uygulaması ve Windows çapraz derlemesi başarılıdır; Windows'ta görüşme sesi köprüsü henüz yoktur.

Windows uygulaması için HFP Hands-Free ses yolu henüz uygulanmadı. [Microsoft'un Bluetooth Classic ses belgeleri](https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-classic-audio) Windows'un standart HFP ses uçlarının bağlı kulaklık/aksesuar için Audio Gateway rolünü tanımlar; telefonun Audio Gateway olduğu ters rol ayrıca araştırılmalıdır. WinBoat sanal makinesi gerçek Bluetooth donanımını geçirmiyorsa test fiziksel Windows makinede yapılacaktır.

## Diğer ses akışları

“Oynatıcı sesini aktar” seçili uygulama oynatırken eşleşmiş Linux bilgisayara Bluetooth
A2DP profili üzerinden telefonun medya çıkışını verir. Android MediaSession durumu mevcut
bildirim erişimiyle okunur. “Ekran paylaşımı sesi” ise ekran oturumunda QUIC üzerinden
PCM taşır. Bu üç akışın kontrolleri ayrıdır.
