# AirLink Protokolü (v1)

Bu belge, AirLink cihazlarının birbirini nasıl bulduğunu, nasıl eşleştiğini ve nasıl veri alışverişi yaptığını tanımlar.

## 1. Kimlik

- Her cihaz ilk çalıştırmada kendinden imzalı bir **ECDSA P-256 sertifikası** üretir ve saklar.
- **Cihaz kimliği (device id)** = sertifikanın DER kodlamasının SHA-256 özeti (64 hex karakter).
- Kullanıcıya kısa hali (ilk 8 karakter) gösterilir.
- Kimlik taklit edilemez: TLS el sıkışması, karşı tarafın bu sertifikanın özel anahtarına sahip olduğunu kanıtlar.

## 2. Keşif (mDNS / DNS-SD)

- Servis türü: `_airlink._udp.local.`
- Örnek adı: cihaz kimliğinin kısa hali
- TXT kayıtları:

| Anahtar | Değer |
|---|---|
| `id` | tam cihaz kimliği |
| `name` | kullanıcının verdiği cihaz adı |
| `platform` | `linux` / `windows` / `android` / `macos` |
| `v` | protokol sürümü (`1`) |

mDNS'in engellendiği ağlarda doğrudan `ip:port` ile de bağlanılabilir. Varsayılan port **47820/UDP**'dir.

## 3. Taşıma katmanı

- **QUIC** (TLS 1.3) kullanılır, ALPN değeri `airlink/1`.
- **Karşılıklı TLS**: her iki taraf da sertifikasını sunar. Sertifikalar bir CA'ya göre doğrulanmaz. Güven, eşleştirme sırasında kaydedilen cihaz kimliğine dayanır.
- Her cihaz tek bir UDP soketi kullanır, hem dinler hem bağlanır. Böylece gelen bağlantının kaynak adresi, karşı cihazın dinleme adresidir.
- Her işlem (eşleştirme, dosya aktarımı, pano) **ayrı bir çift yönlü QUIC akışında** yürür.

### Çerçeveleme

Her kontrol mesajı şu formatta gönderilir:

```
[u32 big-endian uzunluk][UTF-8 JSON]
```

- JSON'daki `type` alanı mesaj türünü belirtir.
- Bir başlık en fazla 8 MiB olabilir.
- Dosya içerikleri JSON değil, ham bayt olarak aynı akışta gönderilir.

## 4. Bağlantı açılışı (Hello)

Bağlanan taraf ilk akışı açar ve `Hello` gönderir. Sunucu da `Hello` ile yanıt verir.

```json
{"type":"hello","device":{"id":"…","name":"Kasım-PC","platform":"linux","version":"0.1.0","port":47820}}
```

`device.id` alanına güvenilmez. Gerçek kimlik her zaman TLS sertifikasından hesaplanır.

## 5. Eşleştirme (SAS: kısa doğrulama kodu)

Bluetooth "Numeric Comparison" yönteminin aynısıdır.

- **A** bağlantıyı başlatan cihazdır, **B** yanıt verendir.
- `fpA` ve `fpB` TLS sertifikalarından hesaplanan kimliklerdir.
- `nA` ve `nB` 32 baytlık rastgele sayılardır.

| Adım | Yön | Mesaj |
|---|---|---|
| 1 | A → B | `pair_request` |
| 2 | B → A | `pair_commit { commitment = SHA256("airlink-commit-v1" ‖ fpA ‖ fpB ‖ nB) }` |
| 3 | A → B | `pair_nonce { nonce = nA }` |
| 4 | B → A | `pair_reveal { nonce = nB }`. A, taahhüdü (commitment) doğrular. |
| 5 | her ikisi | `kod = u32(SHA256("airlink-sas-v1" ‖ fpA ‖ fpB ‖ nA ‖ nB)[0..4]) mod 10⁶`. 6 haneli kod iki ekranda da gösterilir. |
| 6 | her ikisi | `pair_confirm { accepted }`: kullanıcı kodların aynı olduğunu onaylar. |

İki taraf da onaylarsa her cihaz karşı tarafın kimliğini güvenilir cihazlar listesine kaydeder.

**Neden güvenli?** B, A'nın rastgele sayısını görmeden önce kendi sayısına bağlanır (taahhüt). A da B'nin sayısını görmeden önce kendi sayısını gönderir. Bu yüzden araya giren bir saldırgan kodu önceden ayarlayamaz. Bir deneme ile başarılı olma şansı 1/1.000.000'dur.

## 6. Yetkilendirme

- Eşleştirme dışındaki her istek için karşı cihazın güvenilir listede olması gerekir. Değilse sunucu `error { message: "not_paired" }` döner ve akışı kapatır.
- İstemci de bilinen bir cihaza bağlandığında sertifika kimliğinin beklenen kimlikle aynı olduğunu kontrol eder.

## 7. Dosya aktarımı

| Adım | Yön | Mesaj |
|---|---|---|
| 1 | A → B | `file_offer { transfer_id, files: [{ path, size }] }` |
| 2 | B → A | `file_offer_response { accepted }` |
| 3 | A → B | (kabul edildiyse) dosyaların ham baytları, sırayla ve arka arkaya |
| 4 | A → B | `file_done { sha256: [hex, …] }` |
| 5 | B → A | `file_ack { ok, error? }` |

- `path` göreli bir yoldur ve `/` ile ayrılır. Bu sayede klasör de gönderilebilir.
- Alıcı yolu temizler: `..`, `.`, boş parçalar ve geçersiz karakterler atılır.
- Dosya önce `.part` uzantısıyla yazılır. Özet doğrulanınca asıl adına çevrilir.
- Aynı isimde dosya varsa sonuna ` (1)`, ` (2)`… eklenir.

## 8. Pano

| Adım | Yön | Mesaj |
|---|---|---|
| 1 | A → B | `clipboard { text, sensitive? }` (en fazla 1 MiB; `sensitive: true` bir şifre: alıcı gizli gösterir ve kısa süre tutar, eski sürümler alanı göndermez) |
| 2 | B → A | `ack` |

## 9. Uygulama mesajları

Dosya ve pano dışındaki özellikler (bildirimler, SMS, aramalar…) çekirdeğin bilmediği uygulama mesajlarıyla taşınır:

| Adım | Yön | Mesaj |
|---|---|---|
| 1 | A → B | `app_message { kind, body }`: `kind` mesajın adı, `body` herhangi bir JSON değeri (en fazla 1 MiB) |
| 2 | B → A | `ack` |

Yalnızca eşleşmiş cihazlar arasında gider. Tanımlı türler:

| `kind` | Yön | `body` |
|---|---|---|
| `notification.posted` | telefon → PC | `{ key, app, package, title, text, time, can_reply }` |
| `notification.removed` | telefon → PC | `{ key }` |
| `notification.dismiss` | PC → telefon | `{ key }` |
| `notification.reply` | PC → telefon | `{ key, text }` |
| `sms.threads_request` | PC → telefon | `{}` → `sms.threads { threads: [{ thread_id, address, name, snippet, date, unread }] }` |
| `sms.thread_request` | PC → telefon | `{ thread_id }` → `sms.messages { thread_id, messages: [{ id, body, date, outgoing, failed }] }` |
| `sms.send` | PC → telefon | `{ address, text, client_id? }` → `sms.sent { client_id, address, ok, error? }` |
| `sms.received` | telefon → PC | `{ address, name, body, date, thread_id }` |
| `sms.error` | telefon → PC | `{ error, request }` (`no_permission`: SMS izni verilmemiş) |
| `contacts.search` | PC → telefon | `{ query }` → `contacts.results { query, contacts: [{ name, number, label }] }` |
| `contacts.list` | PC → telefon | `{ query, offset }` → `contacts.page { query, offset, next_offset, has_more, contacts: [{ name, number, label }], error? }` (en çok 100 satır/sayfa) |
| `calls.log_request` | PC → telefon | `{ offset }` → `calls.log { offset, next_offset, has_more, calls: [{ number, name, type, date, duration }] }` (en çok 80 kayıt/sayfa); izin yoksa `calls.error { error: "no_permission" }` |
| `call.ringing` | telefon → PC | `{ number, name }` |
| `call.state` | telefon → PC | `{ state }`: `offhook` (yanıtlandı/aranıyor) veya `idle` (bitti) |
| `call.status_request` | PC → telefon | `{}` → `call.status` |
| `call.status` | telefon → PC | `{ full_control, state, held, can_hold, muted, route, routes, number?, name?, since?, volume, volume_max }`; `state`: `ringing`, `dialing`, `active`, `held`, `ended`, `other`. `full_control` yalnızca AirLink varsayılan arama uygulamasıyken (`InCallService`) `true` olur |
| `call.answer` / `call.reject` | PC → telefon | `{}` |
| `call.hangup` | PC → telefon | `{}` |
| `call.hold` / `call.mute` | PC → telefon | `{ on }` (bekletme yalnızca tam denetimde) |
| `call.route` | PC → telefon | `{ route }`: `earpiece`, `speaker`, `bluetooth`, `wired` |
| `call.volume` | PC → telefon | `{ delta }`: pozitif artırır, diğerleri azaltır |
| `call.dtmf` | PC → telefon | `{ digit }`: `0`–`9`, `*`, `#`; tuş tonu görüşmeye çalınır (yalnızca tam denetimde) |
| `media.state_request` | PC → telefon | `{}` → `media.state` |
| `media.state` | telefon → PC | `{ active, app, package, title, artist, album, playing, position, position_at, duration, can_next, can_previous, art }`; çalan medya değişince tüm bilgisayarlara gider. `position_at` ms (epoch), `art` 160 px JPEG (base64) |
| `media.control` | PC → telefon | `{ action }`: `play`, `pause`, `toggle`, `next`, `previous` |
| `call.dial` | PC → telefon | `{ number }` → `call.dial_result { number, ok, error? }` (`no_number`, `no_permission`) |
| `call.audio_share` | telefon → Linux PC | `{ on, once? }`; `on: true` Linux bilgisayarda yalnızca aktif görüşme sırasında HFP köprüsünü otomatik açar. `once: true` yalnızca sürmekte olan görüşmeye uygulanır. `on: false` otomatiği ve etkin köprüyü kapatır |
| `screen.start` | PC → telefon | `{ quality, codec? }` (bkz. bölüm 10) → `screen.started { width, height }` veya `screen.stopped { reason }` |
| `screen.stop` | PC → telefon | `{}` |
| `screen.audio_error` | telefon → PC | `{ reason }`; ses izni reddedildiğinde veya yakalama başarısız olduğunda |
| `player.audio` | Android → Linux | `{ playing }`; seçili oynatıcı çalışırken üç saniyede bir yenilenir. Duraklatmada veya sekiz saniye yenilenmezse A2DP profili bırakılır. |
| `player.audio.status` | Linux → Android | `{ status }`; Bluetooth profilinin bağlantı durumu. |
| `screen.stopped` | telefon → PC | `{ reason }`: `declined`, `failed`, `user`, `computer`… |
| `screen.input` | PC → telefon | `{ type, x, y, x2?, y2?, ms? }`; `type`: `tap`, `long_press`, `swipe`, `back`, `home`, `recents`, `notifications`. Koordinatlar 0–1 arası oranlardır; erişilebilirlik servisi ve etkin ekran paylaşımı gerekir |
| `apps.list` | PC → telefon | `{}` → bir veya daha çok `apps.list_result { list, part, parts, apps: [{ label, component, icon }] }` (`icon` 72 px PNG, base64; parçalar ~400 KiB) |
| `apps.open` | PC → telefon | `{ session, component, width, height, dpi, fps, quality, codec? }` (gelişmiş mod) |
| `apps.close` | PC → telefon | `{ session }` |
| `apps.input` | PC → telefon | `{ session, cmd, … }`; `cmd`: `touch { action: down/move/up, x, y }`, `scroll { x, y, h, v }`, `key { code, meta }` (Android tuş kodu), `text { text }`. Klavye haritasının yazamadığı metin telefon panosundan yapıştırılır |
| `apps.event` | telefon → PC | `{ session, event, message? }`; `event`: `opened`, `closed`, `error` |
| `desktop.start` | izleyen → bilgisayar | `{ max_width?, fps? }`: bilgisayarın ekranını ister. Bilgisayardaki kullanıcı onaylar; Linux'ta masaüstü de ilk seferde ekranı sorar → `desktop.started { width, height }` veya `desktop.stopped { reason, error? }` |
| `desktop.stop` | izleyen → bilgisayar | `{}` |
| `desktop.stopped` | bilgisayar → izleyen | `{ reason, error? }`; `reason`: `declined`, `user` (bilgisayarda durduruldu), `replaced` (başka cihaz başladı), `viewer`, `failed`, `unsupported`, `computer` |
| `desktop.input` | izleyen → bilgisayar | `{ type, … }`; koordinatlar 0–1 arası oranlardır. `move { x, y }`, `button { button?, down, x, y }`, `click { button?, x, y, count? }`, `scroll { x, y, dx?, dy? }` (çentik; pozitif `dy` aşağı), `key { key, down?, modifiers? }`, `text { text }`. `button`: `left` (varsayılan), `right`, `middle`. `key`: `Enter`, `Backspace`, `Escape`, `Tab`, `Delete`, `Space`, ok tuşları, `Home`, `End`, `PageUp`, `PageDown`, `Insert`, `Shift`, `Control`, `Alt`, `Super`, `F1`–`F12` veya tek karakter. Yalnızca paylaşımı alan cihazdan kabul edilir |

## 10. Medya akışları

Ekran görüntüsü gibi sürekli ikili veri, tek yönlü bir QUIC akışında gider:

```
[u32 uzunluk]["media" başlığı: { "type": "media", "name": "screen" }]
[u32 uzunluk][kare baytları] [u32 uzunluk][kare baytları] ...
```

Alıcı her akışın yalnızca en son karesini tutar; gönderen, yazılamamış bir kareyi yenisi gelince atar.
Böylece yavaş bir bağlantıda gecikme birikmez. Akışın başlatılması ve durdurulması uygulama mesajlarıyla
istenir (`screen.start`, `screen.stop`); dokunuşlar da öyle gider (`screen.input`).

`screen.start` ve `apps.open` istekleri `"codec":"h264"` içerebilir. Android bu durumda MediaCodec ile
her kareyi bağımsız bir H.264 anahtar karesi olarak kodlar. Kare baytları `ALH1` ASCII öneki,
ardından Annex B biçimindeki SPS/PPS ve IDR birimlerinden oluşur. Bağımsız kareler,
yukarıdaki son kareyi tutma davranışında referans karesi kaybını önler. Eski Android sürümü
istekteki alanı yok sayıp JPEG gönderebilir; çözücü JPEG ve `ALH1` karelerini ayırt eder.
Telefonda kodlayıcı başlatılamazsa JPEG gönderilir. H.264 kodlama ve çözme yolu Android
emülatöründe ve fiziksel telefonda doğrulandı; uygulama pencereleri ve ekran paylaşımı gerçek
telefonla canlı çalışıyor.

Akış adları: telefon ekranı `screen`, gelişmiş moddaki her uygulama oturumu `app:<session>`,
bilgisayarın ekranı `desktop` (Faz 4; bilgisayar `ALH1` H.264 anahtar kareleri gönderir: Linux'ta
xdg-desktop-portal ScreenCast + PipeWire, Windows'ta GDI; kodlayıcı OpenH264).

Oynatıcı sesi, Android'in mevcut bildirim erişimiyle okunan MediaSession durumuna göre
başlatılır. Yalnızca “Oynatıcıları seç” menüsünde kaydedilen uygulamalar oynatmayı
tetikler. QUIC yalnızca oynatma durumunu taşır. Bluetooth A2DP, telefonun medya ses
çıkışını Linux bilgisayara gönderir. Oynatıcı sesi sistem ekran paylaşımı veya ses
yakalama izni gerektirmez. HFP görüşmesi etkinleştiğinde A2DP askıya alınır.

Ekran paylaşımının sesi, gönderen cihazdaki “Ekran paylaşımı sesi” ayarına bağlı ayrı
bir PCM akışıdır. Telefon tek uygulama veya tam ekran görüntüsü paylaşırken `screen_audio`,
Linux veya Windows bilgisayar ekran paylaşırken `desktop_audio` kullanır. Her ikisi de
48 kHz mono PCM16 taşır. Telefon Android'in yakalamaya izin verdiği medya karışımını,
bilgisayar varsayılan ses çıkışını gönderir. Alıcı yalnızca görüntüsü açık olan eşin
ekran sesi akışını çalar. Görüşme sesi Linux'ta ayrı Bluetooth HFP yolunu kullanır.

Paylaşım kapsamı yalnızca Android'in sistem seçicisinden onaylanır. Android 14 ve
sonrasında sistem arayüzünün seçtiği kapsam veya uygulama MediaProjection üzerinden
bildirilmez. “Müzik oynatılırken ekran sesini durdur” açıksa, kaydedilmiş bir
oynatıcı oynatma durumuna girdiğinde ekran sesi durur ve oynatma bitince geri döner.
Bildirim erişimiyle oynatmayı izleme, Bluetooth aktarımı kapalıyken de çalışır.
Ayar kapalıysa ekran sesi, oturum süresince önceliklidir. Etkin ekran sesi akışı
kaynak cihazın yerel oynatımını susturur veya geçici bir çıkışa taşır.

## 11. Bilinmeyen mesajlar

Bilinmeyen bir `type` gelirse yanıt `error { message: "unsupported" }` olur.
