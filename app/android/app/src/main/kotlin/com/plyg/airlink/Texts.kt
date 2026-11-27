package com.plyg.airlink

import android.content.Context
import java.io.File
import java.util.Locale

/**
 * AirLink's own Android texts (notifications, toasts, dialogs) in the UI's language, as the app's
 * tr()/trf() (app/src/i18n.cpp): written in Turkish, English from the table below. The language
 * is the setting the app keeps in filesDir/language ("system", "tr" or "en").
 */
object Texts {
    @Volatile private var dir: File? = null

    /** Where the setting is: called as the app, the service or the accessibility service starts. */
    fun init(context: Context) {
        dir = context.applicationContext.filesDir
    }

    private fun english(): Boolean {
        val setting = dir?.let { runCatching { File(it, "language").readText().trim() }.getOrNull() } ?: "system"
        return when (setting) {
            "en" -> true
            "tr" -> false
            else -> Locale.getDefault().language != "tr"
        }
    }

    fun tr(turkish: String): String = if (english()) ENGLISH[turkish] ?: turkish else turkish

    /** [tr] with each {} filled with the values in order; in English, [one|many] after a {} picks by it. */
    fun trf(turkish: String, vararg values: Any): String {
        val pattern = tr(turkish)
        val out = StringBuilder()
        var next = 0
        var last = ""
        var i = 0
        while (i < pattern.length) {
            when {
                pattern.startsWith("{}", i) -> {
                    last = values.getOrNull(next++)?.toString() ?: ""
                    out.append(last)
                    i += 2
                }
                pattern[i] == '[' && pattern.indexOf('|', i) in i..pattern.indexOf(']', i) -> {
                    val bar = pattern.indexOf('|', i)
                    val close = pattern.indexOf(']', i)
                    out.append(if (last == "1") pattern.substring(i + 1, bar) else pattern.substring(bar + 1, close))
                    i = close + 1
                }
                else -> out.append(pattern[i++])
            }
        }
        return out.toString()
    }

    private val ENGLISH = mapOf(
        // The service and requests
        "AirLink hazır" to "AirLink is ready",
        "Cihazlarınız dosya ve pano gönderebilir" to "Your devices can send files and the clipboard",
        "Durdur" to "Stop",
        "AirLink durumu" to "AirLink status",
        "Eşleşme ve dosya istekleri" to "Pairing and file requests",
        "{} eşleşmek istiyor" to "{} wants to pair",
        "Kod: {} {} — diğer cihazdakiyle aynıysa onaylayın" to "Code: {} {} — confirm if it matches the other device",
        "Kodlar aynı" to "Codes match",
        "{} dosya göndermek istiyor" to "{} wants to send files",
        "{} dosya, {} ve diğerleri" to "{} files, {} and others",
        "Kabul et" to "Accept",
        "Reddet" to "Decline",
        // Received files and clipboard
        "Gelen dosyalar ve pano" to "Received files and clipboard",
        "{} bir şifre gönderdi" to "{} sent a password",
        "Panoya kondu (gizli)" to "Put on the clipboard (hidden)",
        "{} panosunu paylaştı" to "{} shared its clipboard",
        "{} bir dosya gönderdi" to "{} sent a file",
        "{} {} dosya gönderdi" to "{} sent {} files",
        "Aç" to "Open",
        "Paylaş" to "Share",
        "Dosya bulunamadı" to "File not found",
        "Bu dosyayı açacak uygulama yok" to "No app can open this file",
        "Panoda şifre var" to "A password is on the clipboard",
        "Güvenilir cihazlarınıza gönderilsin mi?" to "Send it to your trusted devices?",
        "Gönder" to "Send",
        "Gönderme" to "Don't send",
        // Find my phone
        "Telefonumu bul" to "Find my phone",
        "Telefonunuz aranıyor" to "Your phone is being looked for",
        "Bir bilgisayardan çaldırıldı" to "Rung from a computer",
        "Buldum" to "Found it",
        // Screen sharing and audio
        "Bilgisayar ekranınızı görmek istiyor" to "A computer wants to see your screen",
        "Başlatmak için dokunun" to "Tap to start",
        "Medya sesini paylaş" to "Share media audio",
        "Ses izni vermek için dokunun" to "Tap to allow audio",
        "Ekran paylaşımı olmadan ses gönderebilen oynatıcılar" to "Players that can send audio without screen sharing",
        "Vazgeç" to "Cancel",
        "Kaydet" to "Save",
        "Ses ekran paylaşımı üzerinden aktarılıyor" to "Audio goes through the screen share",
        "Oynatmayı algılamak için AirLink bildirim erişimi gerekiyor" to "Detecting playback needs AirLink's notification access",
        "Oynatıcıları seç menüsünden bir uygulama seçin" to "Pick an app with Pick players",
        "Bluetooth medya bağlantısı bekleniyor" to "Waiting for the Bluetooth media connection",
        "Seçili oynatıcıların oynatması bekleniyor" to "Waiting for a chosen player to play",
        // Fling and sharing
        "Paylaşılan içerik okunamadı" to "Couldn't read the shared content",
        "Ekrandaki içeriği bulmak için Erişilebilirlik'te AirLink'i açın" to "Turn on AirLink in Accessibility to find what's on screen",
        "Gönderilecek içerik bulunamadı" to "Nothing to send was found",
        "Paylaşım menüsünde AirLink bulunamadı; menüde AirLink'i sabitleyin" to "AirLink isn't in the share menu; pin AirLink there",
        // Advanced mode
        "Eşleme kodunu buraya yazın" to "Type the pairing code here",
        "Kablosuz hata ayıklama > Eşleme koduyla cihaz eşle. Kodu girin; bağlantı bulunamazsa kod:eşleme_portu yazın." to
            "Wireless debugging > Pair device with pairing code. Enter the code; if no connection is found, type code:pairing_port.",
        "6 haneli kod veya kod:port" to "6-digit code or code:port",
        "Kodu gir" to "Enter code",
        "Gelişmiş mod hazır" to "Advanced mode is ready",
        "AirLink artık telefon ekranını uygulama uygulama bilgisayara açabilir." to "AirLink can now open the phone's apps one by one on the computer.",
        "Eşleşme olmadı" to "Pairing didn't work",
        "{} — kodu yeniden girin." to "{} — enter the code again.",
        "Eşleniyor…" to "Pairing…",
        "Kablosuz hata ayıklama ile eşleşiliyor." to "Pairing with wireless debugging.",
        "Gelişmiş moda bağlanılamadı: {}" to "Couldn't connect to advanced mode: {}",
        "Yardımcı başlamadı (/data/local/tmp/airlink-helper.log)" to "The helper didn't start (/data/local/tmp/airlink-helper.log)",
    )
}
