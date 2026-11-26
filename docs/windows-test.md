# AirLink Windows / WinBoat denemesi

1. `app/build-windows/AirLink-Windows-ANGLE.zip` dosyasını Windows'a kopyalayıp bir klasöre açın. `airlink.exe`, `libEGL.dll`, `libGLESv2.dll`, `fonts/` ve `licenses/` aynı klasörde kalmalı. ANGLE, Vulkan sürücüsü olmayan sanal Windows ekranında Direct3D 11 üzerinden çizim sağlar.
2. Linux veya Android'de AirLink açıkken Windows'ta `airlink.exe` çalıştırın. Cihazlar iki tarafta görünüyorsa eşleştirin ve altı haneli kodu karşılaştırıp onaylayın.
3. Windows'tan küçük bir dosya gönderin ve ters yönde bir dosya alın. Ardından birkaç dosyayı birlikte gönderin. Dosya seçici, sürükle bırak ve Explorer'da bir dosyaya sağ tıklayıp **AirLink ile gönder** yolunu deneyin. Explorer'daki **Gönder** menüsündeki AirLink kısayolunu da deneyin.
4. Cihaz görünmezse Windows'ta **Cihazlar → IP adresiyle eşleştir** alanına Linux AirLink'in bulunduğu bilgisayarın IPv4 adresini yazın. Linux'ta `ip -br addr` adresi gösterir; AirLink'in "Bu cihaz" kartı bağlantı noktasını gösterir (varsayılan 47820). Örnek: `192.168.0.10:47820`. WinBoat ağ yalıtımı mDNS keşfinin geçmesini engelleyebilir. IP ile eşleştirdikten sonra dosyaları iki yönde deneyin.

Explorer menüsünü kaldırmak için uygulama kapalıyken aynı klasörde `airlink.exe --unregister` çalıştırın.

WinBoat üzerindeki Windows denemesinde ANGLE ile pencere ve sekmeler, Explorer sağ tık menüsü, IP ile eşleşme, Linux ile iki yönlü dosya aktarımı, çoklu dosya aktarımı ve sürükle bırak doğrulandı. WinBoat ağ yalıtımında otomatik cihaz keşfi çalışmadı. Fiziksel Windows bilgisayarda deneme yapıldı ve çalışıyor.

## Pencere açılmazsa

`AirLink/diagnose.cmd` dosyasını çalıştırın. Başlatıcı OpenGL ES/ANGLE yolunu seçer, mevcut `airlink.exe` işlemlerini ve çıkış kodunu gösterir; aynı klasörde `AirLink-diagnostic.log` dosyası oluşur. Günlükteki başlangıç ve grafik hatalarını paylaşın. WinBoat uygulama penceresi yerine Windows'un tam masaüstünde çalıştırmak hata penceresini görmeyi kolaylaştırabilir.

Paket, EdgeFirstAI'nin `angle-package` `v2.1.28252` sürümündeki Windows x64 kitaplıklarını ve BSD lisansını içerir. Kaynak ZIP'in SHA-256 değeri `e57ac5ce6fb20f24dde931b96ade3ddfbbb7518a52cf17811d996a97de6b7ef9`; paketleme `app/packaging/windows/package.py` ile tekrarlanabilir.

## Bluetooth sesi (Faz 4C, fiziksel Windows)

Telefon önce Windows **Ayarlar → Bluetooth ve cihazlar** üzerinden bilgisayarla eşleşmiş olmalı. Telefonun Bluetooth adı AirLink'teki adıyla aynıysa o seçilir; değilse ve eşleşmiş tek telefon varsa o kullanılır. Telefona güncel AirLink APK'sı kurulmalı. Eski APK'da hedef olarak yalnızca Linux bilgisayar seçilebiliyordu.

**Oynatıcı sesi (A2DP):** Windows 10 2004 veya sonrası gerekir. Paketsiz `airlink.exe` de yeterlidir. Telefonda **Telefon sesini paylaş** kartında Windows bilgisayarını hedef seçin, **Oynatıcı sesini aktar** anahtarını açın ve seçili oynatıcıda müzik çalın. Ses Windows'un varsayılan çıkışından gelmeli. Duraklatınca bağlantı kapanmalı, devam edince yeniden kurulmalı.

**Arama sesi (HFP):** Windows'un telefon hattı API'si kısıtlı `phoneLineTransportManagement` yetkisi istiyor. Bu yetki yalnızca paketli kurulumda alınabiliyor.
1. **Ayarlar → Sistem → Geliştiriciler için** bölümünden Geliştirici Modu'nu açın.
2. Açılan klasörde `install-package.cmd` çalıştırın. Bu, aynı klasörü "AirLink" paketi olarak kaydeder. Eşleşmeler paketsiz sürümle ortaktır. Kaldırmak için `uninstall-package.cmd` kullanın.
3. `diagnose-package.cmd` ile paketli AirLink'i günlükle başlatın (`AirLink-package.log`). Paketsiz `airlink.exe` açıksa kapatılır.
4. Telefonla bir arama yapın. Görüşme sürerken telefonda **Arama sesini paylaş** anahtarını açın ya da bilgisayarda **Bilgisayardan konuş** düğmesine basın. Windows izin sorarsa izin verin. Karşı tarafın sesi bilgisayar hoparlöründen gelmeli, bilgisayar mikrofonu karşı tarafa ulaşmalı. Kapatınca ses telefona dönmeli.
5. Sonraki aramanın sesinin kendiliğinden bilgisayara gitmediğini de kontrol edin. Günlükte `HFP:` ve `A2DP:` satırlarını paylaşın.

Phone Link (Telefon Bağlantısı) aynı telefonda aramalar için açıksa HFP bağlantısını o tutabilir. Deneme sırasında Phone Link'in aramalar özelliğini kapatın.
