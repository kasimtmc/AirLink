#include "i18n.hpp"

#include <fstream>
#include <string>
#include <unordered_map>

#include <kompoz/kompoz.hpp>

#include "platform.hpp"

namespace airlink {

using kompoz::MutableState;
using kompoz::mutableStateOf;

namespace {

// Turkish → English. In an English text, [one|many] after a {} picks by that value: "1 file", "3 files".
const std::unordered_map<std::string_view, std::string_view>& table() {
    static const std::unordered_map<std::string_view, std::string_view> english = {
        // ---- The app, its tabs and tray ----
        {"Cihazlar", "Devices"},
        {"Dosyalar", "Files"},
        {"Pano", "Clipboard"},
        {"Mesajlar", "Messages"},
        {"Telefon", "Phone"},
        {"Uygulamalar", "Apps"},
        {"Ayarlar", "Settings"},
        {"Temizle", "Clear"},
        {"Geçmişi temizle", "Clear history"},
        {"AirLink · paylaşım duraklatıldı", "AirLink · sharing paused"},
        {"AirLink · işlem hatası", "AirLink · an operation failed"},
        {"AirLink · başlatılıyor", "AirLink · starting"},
        {"AirLink · bağlı cihaz yok", "AirLink · no devices connected"},
        {"AirLink · {} cihaz bağlı", "AirLink · {} [device|devices] connected"},
        {"AirLink'i aç", "Open AirLink"},
        {"Dosya gönder…", "Send files…"},
        {"Panoyu gönder", "Send clipboard"},
        {"Ekranı göster", "Show screen"},
        {"Gelen dosyaları aç", "Open received files"},
        {"Paylaşımı sürdür", "Resume sharing"},
        {"Bağlantıyı yeniden dene", "Retry connection"},
        {"Paylaşımı duraklat", "Pause sharing"},
        {"Çık", "Quit"},
        {"Paylaşım duraklatıldı", "Sharing paused"},
        {"Başlatılamadı:\n{}", "Couldn't start:\n{}"},
        {"Panoda şifre var", "A password is on the clipboard"},
        {"Kopyaladığınız şifre güvenilir cihazlarınıza gönderilsin mi?", "Send the password you copied to your trusted devices?"},
        {"Gönder", "Send"},
        {"Gönderme", "Don't send"},

        // ---- Calls ----
        {"Mikrofonu aç", "Unmute"},
        {"Sessize al", "Mute"},
        {"Aç", "Open"},
        {"Kapat", "Close"},
        {"Aramayı kapat", "End call"},
        {"Küçült", "Minimize"},
        {"Aranıyor…", "Calling…"},
        {"Ses: {} (telefon)", "Audio: {} (phone)"},
        {"Tuş takımını gizle", "Hide keypad"},
        {"Sessizde", "Muted"},
        {"Sessiz", "Mute"},
        {"Tuş takımı", "Keypad"},
        {"Sürdür", "Resume"},
        {"Beklet", "Hold"},
        {"Bilgisayarda", "On computer"},
        {"Bilgisayardan konuş", "Talk on computer"},
        {"Telefona al", "Move to phone"},
        {"Sesi azalt", "Volume down"},
        {"Sesi artır", "Volume up"},
        {"Bekletme ve tuş takımı için telefonda AirLink'in arama yardımcısı olarak bağlanması gerekiyor.",
         "Hold and the keypad need AirLink to be connected as the phone's call companion."},
        {"Gelen arama · {}", "Incoming call · {}"},
        {"Geri", "Back"},
        {"Reddet", "Decline"},
        {"Yanıtla", "Answer"},
        {"Yanıtla, bilgisayardan konuş", "Answer on computer"},
        {"Mesajla reddet", "Decline with message"},
        {"Gizle", "Hide"},
        {"Aramalar", "Calls"},
        {"Rehber", "Contacts"},
        {"Telefon bağlı değil", "No phone connected"},
        {"Aramalar için eşleşmiş bir Android telefon çevrimiçi olmalı.", "Calls need a paired Android phone online."},
        {"Numara", "Number"},
        {"Telefondan ara", "Call from phone"},
        {"Tekrar dene", "Try again"},
        {"Arama kaydı telefondan alınıyor…", "Getting the call log from the phone…"},
        {"Arama kaydı yok.", "No calls."},
        {"Yükleniyor…", "Loading…"},
        {"Rehber için eşleşmiş bir Android telefon çevrimiçi olmalı.", "Contacts need a paired Android phone online."},
        {"Rehberde ara", "Search contacts"},
        {"Rehber telefondan alınıyor…", "Getting contacts from the phone…"},
        {"Kişi bulunamadı.", "No contacts found."},
        {"Ara", "Call"},
        {"✕ Cevapsız", "✕ Missed"},
        {"Arama", "Call"},
        {"Gelen", "Incoming"},
        {"Giden", "Outgoing"},
        {"Cevapsız", "Missed"},
        {"Reddedildi", "Declined"},
        {"Hoparlör", "Speaker"},
        {"Kablolu kulaklık", "Wired headset"},
        {"Akış", "Stream"},
        {"Ahize", "Earpiece"},
        {"Beklemede", "On hold"},
        {"Aranıyor", "Calling"},
        {"Görüşmede", "In call"},
        {"Şu an konuşamıyorum, sonra ararım.", "Can't talk now, I'll call you back."},
        {"Toplantıdayım, birazdan dönerim.", "I'm in a meeting, I'll get back to you soon."},
        {"Yoldayım, sonra ararım.", "I'm on my way, I'll call you later."},
        {"Mesaj atar mısın?", "Can you text me?"},

        // ---- Cloud drives ----
        {"Bulut sürücüleri", "Cloud drives"},
        {"OneDrive, Dropbox ve Google Drive hesaplarınızı bağlayın; her hesap kendi sekmesinde açılır. Giriş tarayıcıda yapılır, oturum sistemin anahtar zincirinde saklanır.",
         "Connect your OneDrive, Dropbox and Google Drive accounts; each opens in its own tab. You sign in in your browser, and the sign-in is kept in the system keyring."},
        {"Kaldır", "Remove"},
        {"{}: tarayıcıda giriş bekleniyor…", "{}: waiting for sign-in in the browser…"},
        {"Vazgeç", "Cancel"},
        {"Hesap kaldırılsın mı?", "Remove the account?"},
        {"AirLink bu hesabın oturumunu bu bilgisayardan siler. Sürücüdeki dosyalarınız olduğu gibi kalır.",
         "AirLink deletes this account's sign-in from this computer. Your files in the drive stay as they are."},
        {"Sürücüde ara", "Search the drive"},
        {"Aramayı temizle", "Clear search"},
        {"Yükle", "Upload"},
        {"Yeni klasör", "New folder"},
        {"Yenile", "Refresh"},
        {"\"{}\" için sonuçlar", "Results for \"{}\""},
        {"Yeniden dene", "Retry"},
        {"Eşleşen dosya yok", "No matching files"},
        {"Bu klasör boş. Dosyaları buraya sürükleyin ya da Yükle'ye basın.", "This folder is empty. Drag files here or press Upload."},
        {"Klasör adı", "Folder name"},
        {"Oluştur", "Create"},
        {"İptal", "Cancel"},
        {"İndir", "Download"},
        {"Cihaza gönder", "Send to device"},
        {"{} cihazına gönder", "Send to {}"},
        {"Çevrimiçi cihaz yok", "No devices online"},
        {"Diğer", "More"},
        {"Yeniden adlandır", "Rename"},
        {"Sil", "Delete"},
        {"Yeni ad", "New name"},
        {"Kaydet", "Save"},
        {"{} silinsin mi?", "Delete {}?"},
        {"Dropbox'tan silinir; Dropbox silinen dosyaları bir süre geri yüklenebilir tutar.",
         "It is deleted from Dropbox; Dropbox keeps deleted files restorable for a while."},
        {"{} çöp kutusuna taşınır; oradan geri alınabilir.", "It goes to {}'s recycle bin, where you can restore it."},
        {"Google dokümanı", "Google document"},
        {"{} / {} kullanılıyor", "{} of {} used"},
        {"{} kullanılıyor", "{} used"},
        {"Yüklendi", "Uploaded"},
        {"İndirildi, cihaza gönderiliyor", "Downloaded, sending to the device"},
        {"İndirildi", "Downloaded"},
        {"Yükleniyor · {}", "Uploading · {}"},
        {"İndiriliyor · {}", "Downloading · {}"},
        {"Bağlanılamadı: internet bağlantısını denetleyin", "Couldn't connect: check the internet connection"},
        {"Bulut sürücüsü işlemi başlatılamadı", "Couldn't start the cloud drive operation"},
        {"Klasörler indirilemiyor; içindeki dosyaları seçin", "Folders can't be downloaded; pick the files in them"},
        {"İndirme başlatılamadı", "Couldn't start the download"},
        {"{} bağlandı: {}", "{} connected: {}"},
        {"Bağlanılamadı: {}", "Couldn't connect: {}"},
        {"Hesap kaldırılamadı: {}", "Couldn't remove the account: {}"},
        {"İndirildi: {}", "Downloaded: {}"},
        {"İndirilemedi: {}", "Couldn't download: {}"},
        {"Yüklenemedi: {}", "Couldn't upload: {}"},
        {"Klasör oluşturulamadı: {}", "Couldn't create the folder: {}"},
        {"Yeniden adlandırılamadı: {}", "Couldn't rename: {}"},
        {"Silinemedi: {}", "Couldn't delete: {}"},
        {"Çöp kutusuna taşındı", "Moved to the recycle bin"},
        {"Giriş başlatılamadı", "Couldn't start the sign-in"},
        {"Yükleme başlatılamadı", "Couldn't start the upload"},

        // ---- Devices ----
        {"{} gönderilsin", "Send {}"},
        {"{} dosya gönderilsin", "Send {} [file|files]"},
        {"Henüz eşleşmiş cihaz yok. Önce bir cihazla eşleşin.", "No paired devices yet. Pair with a device first."},
        {"Hangi cihaza?", "To which device?"},
        {"Ya da telefonu {} cihazına doğru fırlatın.", "Or fling the phone toward {}."},
        {"{} (çevrimdışı)", "{} (offline)"},
        {"Eşleşmiş cihazlar", "Paired devices"},
        {"Aktarımlar", "Transfers"},
        {"Tümünü göster ({})", "Show all ({})"},
        {"Telefon bildirimleri", "Phone notifications"},
        {"Yakındaki cihazlar", "Nearby devices"},
        {"Aranıyor… Diğer cihazda AirLink açık ve aynı ağda olmalı.", "Searching… AirLink must be open on the other device, on the same network."},
        {"IP adresiyle eşleştir", "Pair by IP address"},
        {"Ağ keşfi çalışmıyorsa diğer cihazın IP adresini yazın. Varsayılan bağlantı noktası 47820.",
         "If network discovery doesn't work, type the other device's IP address. The default port is 47820."},
        {"IP adresi veya IP:port", "IP address or IP:port"},
        {"Eşleştir", "Pair"},
        {"Cihaz görünmüyor mu? IP adresiyle eşleştir", "Device not showing? Pair by IP address"},
        {"{} yanıtla", "Reply to {}"},
        {"Yanıt", "Reply"},
        {"{} unutulsun mu?", "Forget {}?"},
        {"Tekrar dosya paylaşmak için yeniden eşleşmeniz gerekecek.", "You'll need to pair again to share files."},
        {"Unut", "Forget"},
        {"Göndermek için bırakın", "Drop to send"},
        {"Çevrimdışı · Güvenilir", "Offline · Trusted"},
        {"Çevrimdışı", "Offline"},
        {"Çevrimiçi · Güvenilir", "Online · Trusted"},
        {"Çevrimiçi · dosyaları buraya sürükleyin", "Online · drag files here"},
        {"Çevrimiçi", "Online"},
        {"Güvenilir: geri al", "Trusted: undo"},
        {"Güvenilir yap", "Make trusted"},
        {"Cihazı unut", "Forget device"},
        {"Dosya gönder", "Send files"},
        {"Çalmayı durdur", "Stop ringing"},
        {"Telefonu bul", "Find my phone"},
        {"Kontrol et", "Control"},
        {"{} güvenilir olsun mu?", "Trust {}?"},
        {"Bu cihazdan gelen dosyalar sorulmadan kaydedilir, bilgisayarı kontrol etme istekleri sorulmadan kabul edilir ve kopyaladığınız metinler ona otomatik gider. Kalkan simgesine yeniden dokunarak istediğiniz zaman geri alabilirsiniz.",
         "Files from this device are saved without asking, its requests to control the computer are accepted without asking, and text you copy goes to it automatically. Tap the shield again to undo this at any time."},
        {"Henüz eşleşmiş cihaz yok", "No paired devices yet"},
        {"Diğer cihazda da AirLink'i açın; aşağıda görününce Eşleş'e basıp iki ekrandaki kodu karşılaştırın.",
         "Open AirLink on the other device too; when it shows below, press Pair and compare the code on both screens."},
        {"Eşleş", "Pair"},
        {"{} → bu cihaz", "{} → this device"},
        {"Bu cihaz → {}", "This device → {}"},
        {"İptal edildi", "Cancelled"},
        {"Karşı cihaz iptal etti", "Cancelled on the other device"},
        {"Hata: {}", "Error: {}"},
        {"Tamamlandı · {}", "Done · {}"},
        {"Karşı tarafın onayı bekleniyor… ({})", "Waiting for the other side to accept… ({})"},
        {"Aktarımı iptal et", "Cancel transfer"},
        {"{} eşleşmek istiyor", "{} wants to pair"},
        {"Diğer cihazda da aynı kod görünüyorsa onaylayın.", "Confirm if the same code shows on the other device."},
        {"Kodlar aynı", "Codes match"},
        {"{} dosya göndermek istiyor", "{} wants to send files"},
        {"… ve {} dosya daha", "… and {} more [file|files]"},
        {"Kabul et", "Accept"},
        {"{} dosya, toplam {}", "{} [file|files], {} in all"},
        {"{} ile eşleş", "Pair with {}"},
        {"Diğer cihazın onayı bekleniyor…", "Waiting for the other device…"},
        {"Bu kod diğer cihazda da görünüyor mu?", "Does this code show on the other device too?"},

        // ---- Files and clipboard ----
        {"Dosya geçmişi boş", "No file history"},
        {"Eşleşmiş cihazlarla gönderdiğiniz ve aldığınız dosyalar burada görünür.", "Files you send to and receive from paired devices show here."},
        {"Dosya yöneticisinde göster", "Show in file manager"},
        {"Paylaş", "Share"},
        {"Uygulamalarla paylaş", "Share with apps"},
        {"Dosyayı sil", "Delete file"},
        {"Geçmişten kaldır", "Remove from history"},
        {"Dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır.", "The file is permanently deleted from this device. Its entry stays in the history."},
        {"Gönderdiğiniz özgün dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır.",
         "The original file you sent is permanently deleted from this device. Its entry stays in the history."},
        {"Pano geçmişi boş", "No clipboard history"},
        {"Eşleşmiş cihazlarla paylaşılan metinler ve bağlantılar burada görünür.", "Text and links shared with paired devices show here."},
        {"Şifre · ", "Password · "},
        {"{} cihazından · {}", "From {} · {}"},
        {"{} cihazına · {}", "To {} · {}"},
        {"Bağlantıyı aç", "Open link"},
        {"Kopyala", "Copy"},
        {"Bir cihaz", "A device"},
        {"{} cihazından", "from {}"},
        {"{} cihazına", "to {}"},
        {"dosyaya artık erişilemiyor", "the file can't be reached any more"},
        {"dosya artık yok", "the file is gone"},

        // ---- Messages ----
        {"Mesajlar için eşleşmiş bir Android telefon çevrimiçi olmalı.", "Messages need a paired Android phone online."},
        {"Bir konuşma seçin", "Pick a conversation"},
        {"Soldaki listeden bir konuşma açın ya da yeni mesaj yazın.", "Open a conversation from the list, or write a new message."},
        {"Mesajlar telefondan alınıyor…", "Getting messages from the phone…"},
        {"Mesaj yok.", "No messages."},
        {"Yeni mesaj", "New message"},
        {"Mesaj", "Message"},
        {"{} · gönderilemedi", "{} · not sent"},
        {"gönderiliyor…", "sending…"},
        {"Kişi ya da numara", "Contact or number"},

        // ---- Screens and apps ----
        {"{} bu bilgisayarı görüyor ve kontrol ediyor", "{} sees and controls this computer"},
        {"Durdur", "Stop"},
        {"Ekran paylaşımı sesi", "Screen share audio"},
        {"Bilgisayarın varsayılan ses çıkışını gönderir.", "Sends the computer's default audio output."},
        {"{} bu bilgisayarı kontrol etmek istiyor", "{} wants to control this computer"},
        {"İzin ver", "Allow"},
        {"Bilgisayarda izin verin…", "Allow it on the computer…"},
        {"Görüntü bekleniyor…", "Waiting for the picture…"},
        {"Bilgisayar ekranı", "Computer screen"},
        {"Bilgisayara yaz", "Type on the computer"},
        {"{} ekranı", "{}'s screen"},
        {"Ana ekran", "Home"},
        {"Son", "Recents"},
        {"Telefonda ekran paylaşımına izin verin…", "Allow screen sharing on the phone…"},
        {"Telefon ekranı", "Phone screen"},
        {"Uygulamalar için eşleşmiş bir Android telefon çevrimiçi olmalı.", "Apps need a paired Android phone online."},
        {"Uygulamalar telefondan alınıyor…", "Getting apps from the phone…"},
        {"Uygulama yok.", "No apps."},
        {"Uygulama ara", "Search apps"},
        {"{} uygulama", "{} [app|apps]"},
        {"{} açılıyor…", "Opening {}…"},
        {"Uygulamaya yaz", "Type into the app"},
        {"Ekranınızı görecek, fare ve klavyeyi kullanabilecek. İstediğiniz zaman buradan durdurabilirsiniz.",
         "It will see your screen and can use the mouse and keyboard. You can stop it here at any time."},
        {"\n\nMasaüstü ilk seferde hangi ekranın paylaşılacağını da soracak.", "\n\nThe first time, the desktop will also ask which screen to share."},

        // ---- Settings ----
        {"Şifre gizliliği", "Password privacy"},
        {"Açıkken parola yöneticisinden kopyalanan şifreler güvenilir cihazlara kendiliğinden gitmez; \"Panoda şifre var, gönderilsin mi?\" diye sorulur. Kapalıyken şifreler de kendiliğinden gider. Her iki durumda da şifreler pano geçmişinde gizli görünür ve kopyalandıktan, panel ya da uygulama kapandıktan sonra silinir.",
         "When on, passwords copied from a password manager don't go to trusted devices by themselves; AirLink asks \"A password is on the clipboard, send it?\". When off, passwords go too. Either way, passwords show hidden in the clipboard history and are deleted once copied or when the panel or the app closes."},
        {"Oturum açılınca başlat", "Start at login"},
        {"Telefon açılınca başlat", "Start when the phone starts"},
        {"AirLink oturum açtığınızda pencere açmadan sistem tepsisinde başlar; cihazlarınız bu bilgisayara hemen bağlanabilir.",
         "AirLink starts in the system tray when you log in, without opening a window; your devices can connect to this computer right away."},
        {"AirLink telefon açıldığında ve güncellendiğinde uygulamayı açmanızı beklemeden arka planda başlar; bilgisayarlarınız telefona hemen bağlanabilir.",
         "AirLink starts in the background when the phone starts or is updated, without waiting for you to open it; your computers can connect to the phone right away."},
        {"Bir izin eksik; bazı özellikler kapalı.", "A permission is missing; some features are off."},
        {"{} izin eksik; bazı özellikler kapalı.", "{} permissions are missing; some features are off."},
        {"Genel", "General"},
        {"İzinler", "Permissions"},
        {"Gizlilik", "Privacy"},
        {"Özellikler", "Features"},
        {"Dil", "Language"},
        {"Sistem dili", "System language"},
        {"Bu cihaz · {} · port {} · sürüm {}", "This device · {} · port {} · version {}"},
        {"Gelen dosyalar: {}", "Received files: {}"},
        {"Pil kısıtlaması", "Battery restriction"},
        {"Android pil tasarrufu için AirLink'i arka planda yavaşlatabilir ya da durdurabilir; ekran kapalıyken dosya, pano ve aramalar gecikebilir. AirLink'in kısıtlamasız çalışmasına izin verin.",
         "To save battery, Android may slow down or stop AirLink in the background; with the screen off, files, the clipboard and calls may be delayed. Let AirLink run unrestricted."},
        {"Kısıtlamayı kaldır", "Remove restriction"},
        {"Arka planda bağlı kal", "Stay connected in the background"},
        {"Xiaomi, uygulama süreci kapandığında AirLink'in yeniden başlamasını engelleyebilir. Ayarlar'da AirLink için arka planda otomatik başlatmayı açın.",
         "Xiaomi may keep AirLink from starting again after its process closes. Turn on autostart in the background for AirLink in Settings."},
        {"Otomatik başlatma ayarları", "Autostart settings"},
        {"Bildirimleri bilgisayarda göster", "Show notifications on the computer"},
        {"Bu telefonun bildirimleri eşleşmiş bilgisayarlara gidiyor; oradan kapatılabilir ya da yanıtlanabilir.",
         "This phone's notifications go to paired computers, where they can be dismissed or answered."},
        {"Bilgisayarlarınız bu telefonun bildirimlerini görsün ve mesajları oradan yanıtlayın. Ayarlar'da AirLink'e bildirim erişimi verin.",
         "Let your computers see this phone's notifications and answer messages from there. Give AirLink notification access in Settings."},
        {"SMS'leri bilgisayarda göster", "Show SMS on the computer"},
        {"Eşleşmiş bilgisayarlar bu telefonun SMS'lerini görebilir ve gönderebilir.", "Paired computers can read and send this phone's SMS."},
        {"Bilgisayarınızdan mesajlarınızı okuyun ve SMS gönderin. AirLink'e SMS (ve isimler için kişiler) izni verin.",
         "Read your messages and send SMS from your computer. Give AirLink the SMS permission (and contacts, for names)."},
        {"Aramaları bilgisayarda göster", "Show calls on the computer"},
        {"Telefon çalınca eşleşmiş bilgisayarlar arayanı görür; oradan yanıtlanabilir, reddedilebilir ya da numara aratılabilir.",
         "When the phone rings, paired computers see who's calling; the call can be answered or declined there, and numbers dialled."},
        {"Kim aradığını bilgisayarda görün, aramayı oradan reddedin ve arama kaydınıza ulaşın. AirLink'e telefon ve arama izinleri verin.",
         "See who's calling on the computer, decline calls from there and reach your call log. Give AirLink the phone and call permissions."},
        {"Ekranı bilgisayardan kontrol et", "Control the screen from a computer"},
        {"Bilgisayar ekranınızı görmek istediğinde onayınız sorulur; paylaşım sürerken oradan dokunabilir ve kaydırabilir.",
         "When a computer wants to see your screen, you're asked first; while it's shared, it can tap and swipe."},
        {"Ekranınız bilgisayarda görünebilir (her seferinde onayınızla). Oradan dokunmak için Erişilebilirlik'te \"AirLink ekran kontrolü\"nü açın.",
         "Your screen can show on a computer (with your approval each time). To tap from there, turn on \"AirLink screen control\" in Accessibility."},
        {"Erişilebilirliği aç", "Open Accessibility"},
        {"İzin ver ", "Allow "},
        {"Telefon sesini paylaş", "Share phone audio"},
        {"Hedef bilgisayar", "Target computer"},
        {"Otomatik bağlan", "Connect automatically"},
        {"Arama sesi açıksa görüşme başladığında Bluetooth bağlantısı kurulur.", "If call audio is on, Bluetooth connects when a call starts."},
        {"Arama sesini paylaş", "Share call audio"},
        {"Süren görüşmenin sesini aktar", "Move the current call's audio"},
        {"Arama sesi paylaşımı Linux ve Windows bilgisayarda kullanılabilir.", "Call audio sharing works with a Linux or Windows computer."},
        {"Tek uygulama ve tam ekran paylaşımında telefonun medya çıkışını gönderir.", "Sends the phone's media output when sharing one app or the whole screen."},
        {"Müzik oynatılırken ekran sesini durdur", "Pause screen audio while music plays"},
        {"Oynatmayı algılamak için bildirim erişimini aç", "Turn on notification access to detect playback"},
        {"Oynatıcı sesini aktar", "Send player audio"},
        {"Bildirim erişimini aç", "Turn on notification access"},
        {"Oynatıcıları seç", "Pick players"},
        {"Seçili bir oynatıcı çalarken eşleşmiş bilgisayara Bluetooth medya sesi gider.", "While a chosen player plays, its audio goes to the paired computer over Bluetooth."},
        {"Fırlatarak gönder", "Fling to send"},
        {"Telefonu bilgisayara doğru kısa ve hızlı bir hareketle itin. AirLink açıkken paylaşılan dosyalar ya da kopyalanan metin, başka bir uygulama açıkken o uygulamanın gösterdiği içerik gider: galerideki fotoğraf ya da video, galeri ve dosya yöneticisinde seçili dosyalar, çalan müzik.",
         "Push the phone toward the computer in a short, quick move. With AirLink open, shared files or copied text go; with another app open, what that app shows: the photo or video in the gallery, files selected in the gallery or file manager, the music playing."},
        {"Hedef: çevrimiçi bilgisayar yok", "Target: no computer online"},
        {"Hedef: {}", "Target: {}"},
        {"Diğer uygulamalardan göndermek için Erişilebilirlik'te \"AirLink ekran kontrolü\" açık olmalı.",
         "Sending from other apps needs \"AirLink screen control\" on in Accessibility."},
        {"Galerideki fotoğrafı doğrudan bulmak için fotoğraf, video ve müzik erişimi gerekir.", "Finding the gallery's photo directly needs photo, video and music access."},
        {"Medya erişimi ver", "Give media access"},
        {"Gelişmiş mod", "Advanced mode"},
        {"Uygulamaları bilgisayarda kendi pencerelerinde açın; gerçek dokunuş ve klavye. Kablosuz hata ayıklama ile bir kez kurulur.",
         "Open apps in their own windows on the computer, with real touch and keyboard. Set up once with wireless debugging."},
        {"Kablosuz hata ayıklama portu (isteğe bağlı)", "Wireless debugging port (optional)"},
        {"Bağlantı bulunamazsa Android > Kablosuz hata ayıklama ekranındaki IP:port değerinin yalnızca portunu girin.",
         "If no connection is found, enter only the port of the IP:port shown on Android's Wireless debugging screen."},
        {"Yeniden kur", "Set up again"},
        {"Kur", "Set up"},
        {"Dene", "Test"},

        // ---- Tray panel ----
        {"Gönderilecek çevrimiçi cihaz yok", "No device online to send to"},
        {"1 dosya {} cihazına gönderildi", "1 file sent to {}"},
        {"{} dosya {} cihazına gönderildi", "{} [file|files] sent to {}"},
        {"Metin {} cihazının panosuna gönderildi", "Text sent to {}'s clipboard"},
        {"Panoda gönderilecek dosya ya da metin yok", "No files or text on the clipboard to send"},
        {"Bağlı cihaz yok", "No devices connected"},
        {"{} cihaz bağlı", "{} [device|devices] connected"},
        {"Ctrl+V: kopyaladığınız dosyalar ya da metin {} cihazına gider", "Ctrl+V: the files or text you copied go to {}"},
        {"Eşleşmiş cihazlarınızdan biri çevrimiçi olunca burada görünür.", "Your paired devices show here when they're online."},
        {"Son mesajlar", "Recent messages"},
        {"Bildirimler", "Notifications"},
        {"Son çağrılar", "Recent calls"},
        {"Kişiler", "Contacts"},
        {"Kişi ya da numara ara", "Search contacts or numbers"},
        {"{} numarasını ara", "Call {}"},
        {"Ctrl+V buraya gönderir", "Ctrl+V sends here"},
        {"Dosyaları buraya sürükleyin", "Drag files here"},
        {"Dosya", "File"},
        {"Kopyalanan dosya", "Copied file"},
        {"Kopyalanan {} dosya", "{} copied [file|files]"},
        {"Önceki", "Previous"},
        {"Duraklat", "Pause"},
        {"Çal", "Play"},
        {"Sonraki", "Next"},
        {" · şarj oluyor", " · charging"},
        {"{} sn", "{} s"},
        {"{} dk {} sn", "{} min {} s"},
        {"AirLink'te onaylayın", "Approve in AirLink"},

        // ---- Messages from the app (snackbar) ----
        {"Karşı taraf reddetti", "The other side declined"},
        {"Cihaz çevrimdışı", "The device is offline"},
        {"Cihaz eşleşmemiş", "The device isn't paired"},
        {"Bilgisayarın bağlanması bekleniyor", "Waiting for the computer to connect"},
        {"İstek karşı taraftan iptal edildi", "The other side cancelled the request"},
        {"{} ile eşleşildi", "Paired with {}"},
        {"{} dosya alındı", "{} [file|files] received"},
        {"{} dosya gönderildi", "{} [file|files] sent"},
        {"Ekran {} ile paylaşılıyor", "Sharing the screen with {}"},
        {"Ekran paylaşılamadı: {}", "Couldn't share the screen: {}"},
        {"{} bir şifre gönderdi", "{} sent a password"},
        {"{} panosunu paylaştı", "{} shared its clipboard"},
        {"Eşleşme başlatılamadı: {}", "Couldn't start pairing: {}"},
        {"Eşleşme başarısız: {}", "Pairing failed: {}"},
        {"Eşleşme reddedildi", "Pairing was declined"},
        {"Pano gönderildi", "Clipboard sent"},
        {"Pano gönderilemedi: {}", "Couldn't send the clipboard: {}"},
        {"Ekran açılamadı: {}", "Couldn't open the screen: {}"},
        {"Eşleşildi, kabuk yetkisi deneniyor…", "Paired, trying shell access…"},
        {"Eşleşme olmadı: {}", "Pairing didn't work: {}"},
        {"Hazır: kabuk yetkisiyle çalışıyor ({})", "Ready: running with shell access ({})"},
        {"Beklenmeyen yanıt: {}", "Unexpected answer: {}"},
        {"Gelişmiş mod hazır", "Advanced mode is ready"},
        {"Yanıt gönderildi", "Reply sent"},
        {"Yanıt gönderilemedi: {}", "Couldn't send the reply: {}"},
        {"SMS gönderilemedi: {}", "Couldn't send the SMS: {}"},
        {"Uygulama açılamadı", "Couldn't open the app"},
        {"Ekran paylaşımı telefonda reddedildi", "Screen sharing was declined on the phone"},
        {"Ekran paylaşımı telefonda durduruldu", "Screen sharing was stopped on the phone"},
        {"Telefon başka bir paylaşım isteğiyle meşgul", "The phone is busy with another sharing request"},
        {"{} arıyor", "{} is calling"},
        {"Telefonda AirLink'e arama izinleri verilmemiş. Telefondaki AirLink'ten izin verin.",
         "AirLink has no call permissions on the phone. Allow them in AirLink on the phone."},
        {"{} telefondan aranıyor", "Calling {} from the phone"},
        {"Telefonda arama izni yok", "No call permission on the phone"},
        {"Aranamadı", "Couldn't call"},
        {"Telefonda AirLink'e rehber izni verilmemiş.", "AirLink has no contacts permission on the phone."},
        {"Telefonda AirLink'e SMS izni verilmemiş. Telefondaki AirLink'ten izin verin.",
         "AirLink has no SMS permission on the phone. Allow it in AirLink on the phone."},
        {"Bilgisayarda reddedildi", "Declined on the computer"},
        {"Bilgisayarda durduruldu", "Stopped on the computer"},
        {"Başka bir cihaz bilgisayarı kontrol etmeye başladı", "Another device started controlling the computer"},
        {"Bu bilgisayar ekranını paylaşamıyor", "This computer can't share its screen"},
        {"Ekran paylaşılamadı", "Couldn't share the screen"},
        {"Bilgisayar ekranı kapandı", "The computer's screen closed"},
        {"Görüşme bitince medya devam edecek", "Media resumes when the call ends"},
        {"Bilgisayarın ses çıkışı yakalanamadı", "Couldn't capture the computer's audio output"},
        {"Ekran paylaşımı için medya sesi alınamadı", "Couldn't get media audio for screen sharing"},
        {"Fırlatma için çevrimiçi bir bilgisayar yok", "No computer online to fling to"},
        {"{} dosya {} cihazına fırlatıldı", "{} [file|files] flung to {}"},
        {"Metin {} cihazına fırlatıldı", "Text flung to {}"},
        {"Gönderilecek bir şey yok: dosya paylaşın ya da metin kopyalayın", "Nothing to send: share a file or copy some text"},
        {"Gönderilecek dosya okunamadı", "Couldn't read the file to send"},
        {"Bildirimdeki alana eşleme kodunu yazın…", "Type the pairing code in the notification…"},
        {"Eşleme portu 1–65535 arasında olmalı", "The pairing port must be 1–65535"},
        {"Eşleme portu geçersiz", "The pairing port isn't valid"},
        {"Eşleniyor…", "Pairing…"},
        {"Kablosuz hata ayıklama portu geçersiz", "The wireless debugging port isn't valid"},
        {"Kablosuz hata ayıklama portu 1–65535 arasında olmalı", "The wireless debugging port must be 1–65535"},
        {"Kablosuz hata ayıklamaya bağlanılıyor…", "Connecting to wireless debugging…"},
        {"Bluetooth oynatıcı sesi için çevrimiçi Linux veya Windows bilgisayarı seçin", "Pick an online Linux or Windows computer for Bluetooth player audio"},
        {"Seçili oynatıcıların oynatması bekleniyor", "Waiting for a chosen player to play"},
        {"Bluetooth telefona bağlanıyor", "Bluetooth is connecting to the phone"},
        {"Bırakılan öğede dosya yok", "The dropped item has no files"},
        {"Pano boş", "The clipboard is empty"},
        {"Dosya silinemedi: {}", "Couldn't delete the file: {}"},
        {"Aktarım zaten bitmiş", "The transfer has already ended"},

        // ---- The desktop's own dialogs and notifications ----
        {"AirLink: dosya gönder", "AirLink: send files"},
        {"AirLink ile gönder", "Send with AirLink"},
        {"{} bir dosya gönderdi", "{} sent a file"},
        {"{} {} dosya gönderdi", "{} sent {} files"},
        {"{} ve {} dosya daha", "{} and {} more [file|files]"},
        {"Panoya kondu (gizli)", "Put on the clipboard (hidden)"},
        {"Klasörde göster", "Show in folder"},
        {"Güvenilir cihazlarınıza gönderilsin mi?", "Send it to your trusted devices?"},
    };
    return english;
}

MutableState<std::string>& setting() {
    static MutableState<std::string> s = mutableStateOf(std::string("system"));
    return s;
}

std::string& settingFile() {
    static std::string path;
    return path;
}

bool systemTurkish() {
    static const bool turkish = platform::systemLanguage().starts_with("tr");
    return turkish;
}

}  // namespace

std::string languageSetting() { return setting().get(); }

void setLanguageSetting(const std::string& value) {
    std::string v = value == "tr" || value == "en" ? value : "system";
    setting().set(v);
    if (!settingFile().empty()) std::ofstream(settingFile()) << v;
}

void loadLanguageSetting(const std::string& path) {
    settingFile() = path;
    std::string saved;
    if (std::ifstream in(path); in >> saved) setting().set(saved == "tr" || saved == "en" ? saved : "system");
}

bool english() {
    const std::string& s = setting().get();
    return s == "en" || (s == "system" && !systemTurkish());
}

std::string tr(std::string_view turkish) {
    if (!english()) return std::string(turkish);
    auto it = table().find(turkish);
    return std::string(it == table().end() ? turkish : it->second);
}

namespace detail {

std::string fill(const std::string& pattern, const std::vector<std::string>& values) {
    std::string out;
    size_t next = 0;
    std::string last;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern.compare(i, 2, "{}") == 0) {
            last = next < values.size() ? values[next++] : std::string();
            out += last;
            ++i;
        } else if (pattern[i] == '[') {
            // [one|many]: by the value before it.
            size_t bar = pattern.find('|', i), close = pattern.find(']', i);
            if (bar == std::string::npos || close == std::string::npos || bar > close) {
                out += pattern[i];
                continue;
            }
            out += last == "1" ? pattern.substr(i + 1, bar - i - 1) : pattern.substr(bar + 1, close - bar - 1);
            i = close;
        } else {
            out += pattern[i];
        }
    }
    return out;
}

}  // namespace detail

}  // namespace airlink
