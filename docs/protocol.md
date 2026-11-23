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
| 1 | A → B | `clipboard { text }` (en fazla 1 MiB) |
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
| `sms.send` | PC → telefon | `{ address, text }` → `sms.sent { address, ok, error? }` |
| `sms.received` | telefon → PC | `{ address, name, body, date, thread_id }` |
| `sms.error` | telefon → PC | `{ error, request }` (`no_permission`: SMS izni verilmemiş) |
| `contacts.search` | PC → telefon | `{ query }` → `contacts.results { query, contacts: [{ name, number, label }] }` |

## 10. Gelecek sürümler (ayrılmış)

Şu mesaj aileleri sonraki fazlarda eklenecek:

- `notification.*`
- `sms.*`
- `call.*`
- `contacts.*`
- `screen.*` (video akışı ve uzaktan giriş)
- `input.*`
- `gesture.*`

Bilinmeyen bir `type` gelirse yanıt `error { message: "unsupported" }` olur.
