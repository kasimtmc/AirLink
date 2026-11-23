// AirLink's UI. The @Kompozable functions are Kotlin, turned into C++ by kompozc; the rest is C++.

#include <kompoz/kompoz.hpp>

#include <algorithm>
#include <ctime>

#include "app_icons.hpp"
#include "app_state.hpp"
#include "json.hpp"
#include "platform.hpp"

using namespace kompoz;
using namespace airlink;

// ---- C++ helpers the composables call ------------------------------------------------------

static App appState() { return App::get(); }

static std::string shortId(const std::string& id) { return id.substr(0, 8); }

static std::vector<std::string> droppedFiles(const DragAndDropEvent& e) { return e.data ? e.data->files : std::vector<std::string>{}; }

// Whether a drag may carry files. While it is over the window only its MIME types are known (the
// files are read at the drop): a list of URIs, or anything that isn't text (Android's drags from
// other apps carry a file's own type, image/jpeg, with a content:// URI).
static bool carriesFiles(const DragAndDropEvent& e) {
    if (!e.data) return false;
    if (!e.data->files.empty()) return true;
    for (const std::string& type : e.data->mimeTypes)
        if (type == "text/uri-list" || type == "x-special/gnome-copied-files" || !type.starts_with("text/")) return true;
    return false;
}

// URIs the drop carries that aren't local paths (Android's content:// ones).
static std::vector<std::string> droppedUris(const DragAndDropEvent& e) {
    std::vector<std::string> uris;
    if (!e.data) return uris;
    for (const auto& item : e.data->toClipEntry().items)
        if (item.uri && !item.uri->starts_with("file://")) uris.push_back(*item.uri);
    return uris;
}

static std::string fileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "14:05" for a time in ms since the epoch, in the local time zone.
static std::string clockTime(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", &local);
    return buf;
}

// The first letter of a name, for an avatar (a whole UTF-8 character).
static std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    size_t n = 1;
    unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    return name.substr(0, n);
}

// "14:05" today, "23.11" another day.
static std::string shortDate(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000), now = std::time(nullptr);
    std::tm a{}, b{};
#if defined(_WIN32)
    localtime_s(&a, &t);
    localtime_s(&b, &now);
#else
    localtime_r(&t, &a);
    localtime_r(&now, &b);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M" : "%d.%m", &a);
    return buf;
}

// "1 dk 05 sn", "42 sn".
static std::string duration(int64_t seconds) {
    if (seconds < 60) return std::to_string(seconds) + " sn";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld dk %02lld sn", (long long)(seconds / 60), (long long)(seconds % 60));
    return buf;
}

// Where a point on the shown screen is on the phone's, as fractions (0..1) of it: the image is
// fitted into `box` (letterboxed), keeping its aspect.
static bool toScreen(Offset p, Size box, int iw, int ih, double& x, double& y) {
    if (box.width <= 0 || box.height <= 0 || iw <= 0 || ih <= 0) return false;
    double scale = std::min(double(box.width) / iw, double(box.height) / ih);
    double w = iw * scale, h = ih * scale, left = (box.width - w) / 2, top = (box.height - h) / 2;
    x = (p.x - left) / w;
    y = (p.y - top) / h;
    return x >= 0 && x <= 1 && y >= 0 && y <= 1;
}

static std::string touchBody(const std::string& type, Offset p, Size box, int iw, int ih) {
    double x, y;
    if (!toScreen(p, box, iw, ih, x, y)) return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"x\":%.4f,\"y\":%.4f}", type.c_str(), x, y);
    return buf;
}

static std::string swipeBody(Offset from, Offset to, Size box, int iw, int ih) {
    double x1, y1, x2, y2;
    toScreen(from, box, iw, ih, x1, y1);
    toScreen(to, box, iw, ih, x2, y2);
    auto clamp = [](double v) { return std::clamp(v, 0.0, 1.0); };
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"type\":\"swipe\",\"x\":%.4f,\"y\":%.4f,\"x2\":%.4f,\"y2\":%.4f,\"ms\":250}", clamp(x1),
                  clamp(y1), clamp(x2), clamp(y2));
    return buf;
}

// A touch on an app's display: fractions of it, clamped (a drag may leave the image).
static std::string appTouch(const std::string& action, Offset p, Size box, int iw, int ih) {
    double x = 0, y = 0;
    toScreen(p, box, iw, ih, x, y);
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"cmd\":\"touch\",\"action\":\"%s\",\"x\":%.4f,\"y\":%.4f}", action.c_str(),
                  std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0));
    return buf;
}

static std::string textBody(const std::string& text) { return "{\"cmd\":\"text\",\"text\":" + json::quote(text) + "}"; }

static int64_t nowMs() { return int64_t(std::time(nullptr)) * 1000; }

// "03:25" (or "1:03:25") since `since` (ms).
static std::string elapsed(int64_t since, int64_t now) {
    int64_t s = std::max<int64_t>(0, (now - since) / 1000);
    char buf[16];
    if (s >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
    else std::snprintf(buf, sizeof buf, "%02lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

static std::string routeLabel(const std::string& route) {
    if (route == "speaker") return "Hoparlör";
    if (route == "bluetooth") return "Bluetooth";
    if (route == "wired") return "Kablolu kulaklık";
    if (route == "streaming") return "Akış";
    return "Ahize";
}

static std::string routeBody(const std::string& route) { return "{\"route\":\"" + route + "\"}"; }

static std::string callStateLabel(const ActiveCall& c) {
    if (c.held || c.state == "held") return "Beklemede";
    if (c.state == "dialing") return "Aranıyor";
    return "Görüşmede";
}

static std::string spacedCode(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

static bool isDesktop() {
#if defined(__ANDROID__)
    return false;
#else
    return true;
#endif
}

// ---- Screens ---------------------------------------------------------------------------------

@Kompozable
fun AirLinkApp() {
    val dark = isSystemInDarkTheme()
    val scheme = if (isDynamicColorAvailable()) {
        if (dark) dynamicDarkColorScheme() else dynamicLightColorScheme()
    } else {
        if (dark) darkColorScheme() else lightColorScheme()
    }
    MaterialTheme(colorScheme = scheme) {
        Home()
    }
}

@Kompozable
fun Home() {
    val app = appState()
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val clipboard = clipboardManager()
    var tab by remember { mutableStateOf(0) }

    // The core's events, applied on the UI thread.
    LaunchedEffect(Unit) {
        while (true) {
            app.pump()
            for (text in app.takeIncomingClipboard()) clipboard.setText(text)
            for (message in app.takeNotices()) scope.launch { snackbar.showSnackbar(message) }
            delay(50)
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("AirLink") },
                actions = {
                    if (app.transfers().any { !it.active() }) {
                        TextButton(onClick = { app.clearFinishedTransfers() }) { Text("Temizle") }
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbar) },
    ) { padding ->
        // A computer also shows a paired phone's SMS, on a tab of its own.
        val tabs = isDesktop() && app.ready()
        Column(modifier = Modifier.fillMaxSize().padding(padding)) {
            val call = app.activeCall()
            if (call != null && isDesktop()) ActiveCallBar(app, call!!)
            if (tabs) {
                PrimaryTabRow(selectedTabIndex = tab) {
                    Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text("Cihazlar") })
                    Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text("Mesajlar") })
                    Tab(selected = tab == 2, onClick = { tab = 2 }, text = { Text("Aramalar") })
                    Tab(selected = tab == 3, onClick = { tab = 3 }, text = { Text("Uygulamalar") })
                }
            }
            Box(modifier = Modifier.fillMaxSize()) {
                if (app.ready() && app.screenPeer().isNotEmpty()) {
                    ScreenView(app)
                } else if (app.ready()) {
                    if (tabs && tab == 1) MessagesScreen(app)
                    else if (tabs && tab == 2) CallsScreen(app)
                    else if (tabs && tab == 3) AppsScreen(app)
                    else DeviceList(app, clipboard)
                } else if (app.startError().isNotEmpty()) {
                    Text("Başlatılamadı:\n${app.startError()}", modifier = Modifier.align(Alignment.Center).padding(24.dp))
                } else {
                    CircularProgressIndicator(modifier = Modifier.align(Alignment.Center))
                }
            }
        }
    }

    IncomingPrompt(app)
    OutgoingPairingDialog(app)
    ShareDialog(app)
    IncomingCallDialog(app)
}

// A paired phone's call in progress: who, for how long, and its controls.
@Kompozable
fun ActiveCallBar(app: App, call: ActiveCall) {
    val cs = MaterialTheme.colorScheme
    var now by remember { mutableStateOf(nowMs()) }
    LaunchedEffect(call.since) {
        while (true) {
            now = nowMs()
            delay(1000)
        }
    }
    Card(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), colors = CardDefaults.cardColors(containerColor = cs.tertiaryContainer)) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(AppIcons.Smartphone, contentDescription = null, tint = cs.onTertiaryContainer)
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(call.title(), style = MaterialTheme.typography.titleMedium, color = cs.onTertiaryContainer)
                    Text("${callStateLabel(call)} · ${elapsed(call.since, now)}", style = MaterialTheme.typography.bodySmall,
                         color = cs.onTertiaryContainer)
                }
                Button(onClick = { app.callControl("call.hangup", "{}") },
                       colors = ButtonDefaults.buttonColors(containerColor = cs.error, contentColor = cs.onError)) { Text("Kapat") }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                FilterChip(selected = call.muted, onClick = { app.callControl("call.mute", if (call.muted) "{\"on\":false}" else "{\"on\":true}") },
                           label = { Text(if (call.muted) "Mikrofon kapalı" else "Mikrofon açık") })
                if (call.fullControl && call.canHold) {
                    FilterChip(selected = call.held, onClick = { app.callControl("call.hold", if (call.held) "{\"on\":false}" else "{\"on\":true}") },
                               label = { Text(if (call.held) "Beklemede" else "Beklet") })
                }
                for (r in call.routes) {
                    FilterChip(selected = call.route == r, onClick = { app.callControl("call.route", routeBody(r)) }, label = { Text(routeLabel(r)) })
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TextButton(onClick = { app.callControl("call.volume", "{\"delta\":-1}") }) { Text("Ses −") }
                    Text("${call.volume}/${call.volumeMax}", style = MaterialTheme.typography.labelMedium, color = cs.onTertiaryContainer)
                    TextButton(onClick = { app.callControl("call.volume", "{\"delta\":1}") }) { Text("Ses +") }
                }
            }
            if (!call.fullControl) {
                Text("Sınırlı kontrol: bekletme ve Bluetooth için telefonun AirLink'i arama yardımcısı olarak bağlaması gerekiyor.",
                     style = MaterialTheme.typography.labelSmall, color = cs.onTertiaryContainer)
            }
        }
    }
}

// A paired phone rings: who, and answer (on the phone) or decline from here.
@Kompozable
fun IncomingCallDialog(app: App) {
    val c = app.incomingCall()
    if (c != null) {
        val call = c!!
        AlertDialog(
            onDismissRequest = { app.dismissCall() },
            icon = { Icon(AppIcons.Smartphone, contentDescription = null) },
            title = { Text("${call.title()} arıyor") },
            text = {
                Text(if (call.name.isNotEmpty()) "${call.number}\n${app.peerName(call.peerId)} çalıyor" else "${app.peerName(call.peerId)} çalıyor")
            },
            confirmButton = { TextButton(onClick = { app.answerCall() }) { Text("Telefonda yanıtla") } },
            dismissButton = { TextButton(onClick = { app.rejectCall() }) { Text("Reddet") } },
        )
    }
}

// Files handed to AirLink from outside (a file manager's "Send with AirLink", Android's share
// sheet): which device gets them.
@Kompozable
fun ShareDialog(app: App) {
    val files = app.share()
    if (files.isNotEmpty() && app.ready()) {
        val paired = app.pairedPeers()
        AlertDialog(
            onDismissRequest = { app.cancelShare() },
            icon = { Icon(AppIcons.Upload, contentDescription = null) },
            title = { Text(if (files.size == 1) "${fileName(files.first())} gönderilsin" else "${files.size} dosya gönderilsin") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (paired.isEmpty()) {
                        Text("Henüz eşleşmiş cihaz yok. Önce bir cihazla eşleşin.")
                    } else {
                        Text("Hangi cihaza?", color = MaterialTheme.colorScheme.onSurfaceVariant)
                        for (peer in paired) {
                            OutlinedButton(onClick = { app.shareTo(peer.id) }, modifier = Modifier.fillMaxWidth(), enabled = peer.online) {
                                Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(18.dp))
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(if (peer.online) peer.name else "${peer.name} (çevrimdışı)", modifier = Modifier.weight(1f))
                            }
                        }
                    }
                }
            },
            confirmButton = { TextButton(onClick = { app.cancelShare() }) { Text("Vazgeç") } },
        )
    }
}

@Kompozable
fun DeviceList(app: App, clipboard: ClipboardManager) {
    val transfers = app.transfers()
    val paired = app.pairedPeers()
    val nearby = app.nearbyPeers()
    val notifications = app.phoneNotifications()
    var forgetting by remember { mutableStateOf("") }
    var replyTo by remember { mutableStateOf(PhoneNotification()) }
    var replyText by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { ThisDeviceCard(app.me()) }

        if (app.canShareNotifications()) {
            item { NotificationAccessCard(app) }
        }
        if (app.canShareSms()) {
            item { SmsAccessCard(app) }
        }
        if (app.canShareCalls()) {
            item { CallAccessCard(app) }
        }
        if (app.canShareScreen()) {
            item { ScreenControlCard(app) }
        }
        if (app.canUseAdvancedMode()) {
            item { AdvancedModeCard(app) }
        }

        if (notifications.isNotEmpty()) {
            item { SectionTitle("Telefon bildirimleri") }
            items(notifications) { n ->
                PhoneNotificationCard(n, app.peerName(n.peerId),
                    onDismiss = { app.dismissPhoneNotification(n) },
                    onReply = { replyTo = n })
            }
        }

        if (transfers.isNotEmpty()) {
            item { SectionTitle("Aktarımlar") }
            items(transfers) { t -> TransferRow(t, app.peerName(t.peerId)) }
        }

        item { SectionTitle("Eşleşmiş cihazlar") }
        if (paired.isEmpty()) {
            item { Hint("Henüz eşleşmiş cihaz yok. Aşağıdan yakındaki bir cihazla eşleşin.", loading = false) }
        } else {
            items(paired) { peer ->
                PairedPeerCard(
                    peer = peer,
                    onSendFiles = { app.pickAndSend(peer.id) },
                    onDropFiles = { paths, uris -> app.sendDropped(peer.id, paths, uris) },
                    onSendClipboard = { app.sendClipboard(peer.id, clipboard.getText()) },
                    onForget = { forgetting = peer.id },
                    onShowScreen = { app.startScreen(peer.id) },
                )
            }
        }

        item { SectionTitle("Yakındaki cihazlar") }
        if (nearby.isEmpty()) {
            item { Hint("Aranıyor… Diğer cihazda AirLink açık ve aynı ağda olmalı.", loading = true) }
        } else {
            items(nearby) { peer -> NearbyPeerCard(peer, onPair = { app.pair(peer.id) }) }
        }
    }

    if (replyTo.key.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { replyTo = PhoneNotification(); replyText = "" },
            title = { Text("${replyTo.title} yanıtla") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(replyTo.text, style = MaterialTheme.typography.bodySmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
                    OutlinedTextField(value = replyText, onValueChange = { replyText = it }, modifier = Modifier.fillMaxWidth(), label = "Yanıt")
                }
            },
            confirmButton = {
                TextButton(onClick = { app.replyPhoneNotification(replyTo, replyText); replyTo = PhoneNotification(); replyText = "" }) { Text("Gönder") }
            },
            dismissButton = { TextButton(onClick = { replyTo = PhoneNotification(); replyText = "" }) { Text("Vazgeç") } },
        )
    }

    if (forgetting.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { forgetting = "" },
            title = { Text("${app.peerName(forgetting)} unutulsun mu?") },
            text = { Text("Tekrar dosya paylaşmak için yeniden eşleşmeniz gerekecek.") },
            confirmButton = { TextButton(onClick = { app.forget(forgetting); forgetting = "" }) { Text("Unut") } },
            dismissButton = { TextButton(onClick = { forgetting = "" }) { Text("Vazgeç") } },
        )
    }
}

@Kompozable
fun ThisDeviceCard(me: DeviceInfo) {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.primaryContainer)) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(52.dp).background(cs.primary, CircleShape), contentAlignment = Alignment.Center) {
                Icon(AppIcons.forPlatform(me.platform), contentDescription = null, tint = cs.onPrimary)
            }
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(me.name, style = MaterialTheme.typography.titleLarge, color = cs.onPrimaryContainer)
                Text("Bu cihaz · ${shortId(me.id)} · port ${me.port}", color = cs.onPrimaryContainer)
                Text("Gelen dosyalar: ${me.downloadDir}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onPrimaryContainer, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Kompozable
fun NotificationAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Bildirimleri bilgisayarda göster", style = MaterialTheme.typography.titleMedium)
            if (app.notificationAccess()) {
                Text("Bu telefonun bildirimleri eşleşmiş bilgisayarlara gidiyor; oradan kapatılabilir ya da yanıtlanabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Bilgisayarlarınız bu telefonun bildirimlerini görsün ve mesajları oradan yanıtlayın. Ayarlar'da AirLink'e bildirim erişimi verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openNotificationAccessSettings() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun SmsAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("SMS'leri bilgisayardan kullan", style = MaterialTheme.typography.titleMedium)
            if (app.smsAccess()) {
                Text("Eşleşmiş bilgisayarlar bu telefonun SMS'lerini görebilir ve gönderebilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Bilgisayarınızdan mesajlarınızı okuyun ve SMS gönderin. AirLink'e SMS (ve isimler için kişiler) izni verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestSmsAccess() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun CallAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Aramaları bilgisayarda göster", style = MaterialTheme.typography.titleMedium)
            if (app.callAccess()) {
                Text("Telefon çalınca eşleşmiş bilgisayarlar arayanı görür; oradan yanıtlanabilir, reddedilebilir ya da numara aratılabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Kim aradığını bilgisayarda görün, aramayı oradan reddedin ve arama kaydınıza ulaşın. AirLink'e telefon ve arama izinleri verin.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestCallAccess() }) { Text("İzin ver") }
            }
        }
    }
}

@Kompozable
fun ScreenControlCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Ekranı bilgisayardan kontrol et", style = MaterialTheme.typography.titleMedium)
            if (app.screenControl()) {
                Text("Bilgisayar ekranınızı görmek istediğinde onayınız sorulur; paylaşım sürerken oradan dokunabilir ve kaydırabilir.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text("Ekranınız bilgisayarda görünebilir (her seferinde onayınızla). Oradan dokunmak için Erişilebilirlik'te \"AirLink ekran kontrolü\"nü açın.",
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openScreenControlSettings() }) { Text("Erişilebilirliği aç") }
            }
        }
    }
}

@Kompozable
fun AdvancedModeCard(app: App) {
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Gelişmiş mod", style = MaterialTheme.typography.titleMedium)
            Text("Uygulamaları bilgisayarda kendi pencerelerinde açın; gerçek dokunuş ve klavye. Kablosuz hata ayıklama ile bir kez kurulur.",
                 color = cs.onSurfaceVariant)
            if (app.advancedStatus().isNotEmpty()) {
                Text(app.advancedStatus(), style = MaterialTheme.typography.bodySmall,
                     color = if (app.advancedReady()) Color(0xFF2E7D32) else cs.primary)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = { app.startAdvancedSetup() }) { Text(if (app.advancedReady()) "Yeniden kur" else "Kur") }
                TextButton(onClick = { app.advancedTest() }) { Text("Dene") }
            }
        }
    }
}

// ---- A paired phone's screen (on a computer) ------------------------------------------------

@Kompozable
fun ScreenView(app: App) {
    val frame = app.screenFrame()
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var dragFrom by remember { mutableStateOf(Offset(0f, 0f)) }
    var dragTo by remember { mutableStateOf(Offset(0f, 0f)) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("${app.peerName(app.screenPeer())} ekranı", style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { app.screenInput("{\"type\":\"back\"}") }) { Text("Geri") }
            TextButton(onClick = { app.screenInput("{\"type\":\"home\"}") }) { Text("Ana ekran") }
            TextButton(onClick = { app.screenInput("{\"type\":\"recents\"}") }) { Text("Son") }
            Button(onClick = { app.stopScreen() }) { Text("Kapat") }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(if (app.screenStatus() == "asking") "Telefonda ekran paylaşımına izin verin…" else "Görüntü bekleniyor…", color = Color.White)
                }
            } else {
                Image(frame, contentDescription = "Telefon ekranı", contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .pointerInput(Unit) {
                              detectTapGestures(
                                  onTap = { p -> app.screenInput(touchBody("tap", p, box, frame.width(), frame.height())) },
                                  onLongPress = { p -> app.screenInput(touchBody("long_press", p, box, frame.width(), frame.height())) },
                              )
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> dragFrom = p; dragTo = p },
                                  onDrag = { change, amount -> dragTo = dragTo + amount },
                                  onDragEnd = { app.screenInput(swipeBody(dragFrom, dragTo, box, frame.width(), frame.height())) },
                              )
                          })
            }
        }
    }
}

// ---- A paired phone's apps, each on a display of its own (advanced mode, on a computer) -----

@Kompozable
fun AppsScreen(app: App) {
    val phone = app.smsPhone()
    val apps = app.phoneApps()
    val windows = app.appWindows()
    val active = app.activeWindow()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty() && apps.isEmpty()) app.requestPhoneApps()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint("Uygulamalar için eşleşmiş bir Android telefon çevrimiçi olmalı.", loading = false)
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            if (windows.isNotEmpty()) {
                FlowRow(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment) {
                    for (w in windows) {
                        FilterChip(selected = w.session == active, onClick = { if (w.session == active) app.minimizeAppWindow(w.session) else app.showAppWindow(w.session) },
                                   label = { Text(if (w.minimized) "${w.label} (küçültüldü)" else w.label) })
                    }
                }
            }
            val shown = windows.firstOrNull { it.session == active }
            if (shown != null) {
                AppWindowView(app, shown!!)
            } else {
                AppGrid(app, apps)
            }
        }
    }
}

@Kompozable
fun AppGrid(app: App, apps: List<PhoneApp>) {
    if (apps.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint(if (app.phoneAppsLoading()) "Uygulamalar telefondan alınıyor…" else "Uygulama yok.", loading = app.phoneAppsLoading())
        }
    } else {
        LazyVerticalGrid(GridCells.Adaptive(104.dp), modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp)) {
            items(apps) { a ->
                Column(modifier = Modifier.padding(4.dp).clickable(onClick = { app.openPhoneApp(a) }).padding(8.dp),
                       horizontalAlignment = Alignment.CenterHorizontally) {
                    if (!a.icon.empty()) Image(a.icon, contentDescription = null, modifier = Modifier.size(52.dp))
                    Spacer(modifier = Modifier.height(6.dp))
                    Text(a.label, style = MaterialTheme.typography.bodySmall, maxLines = 2, overflow = TextOverflow.Ellipsis,
                         textAlign = TextAlign.Center)
                }
            }
        }
    }
}

@Kompozable
fun AppWindowView(app: App, w: AppWindow) {
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var last by remember { mutableStateOf(Offset(0f, 0f)) }
    var typed by remember { mutableStateOf("") }
    val fw = if (w.frame.empty()) 720 else w.frame.width()
    val fh = if (w.frame.empty()) 1560 else w.frame.height()
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(w.label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":4}") }) { Text("Geri") }
            TextButton(onClick = { app.minimizeAppWindow(w.session) }) { Text("Küçült") }
            Button(onClick = { app.closeAppWindow(w.session) }) { Text("Kapat") }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (w.frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text("${w.label} açılıyor…", color = Color.White)
                }
            } else {
                Image(w.frame, contentDescription = w.label, contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .pointerInput(Unit) {
                              detectTapGestures(onTap = { p ->
                                  app.appInput(w.session, appTouch("down", p, box, fw, fh))
                                  app.appInput(w.session, appTouch("up", p, box, fw, fh))
                              })
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> last = p; app.appInput(w.session, appTouch("down", p, box, fw, fh)) },
                                  onDrag = { change, amount -> last = last + amount; app.appInput(w.session, appTouch("move", last, box, fw, fh)) },
                                  onDragEnd = { app.appInput(w.session, appTouch("up", last, box, fw, fh)) },
                              )
                          })
            }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true, label = "Uygulamaya yaz", modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, textBody(typed)); typed = "" }, enabled = typed.isNotEmpty()) { Text("Gönder") }
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":66}") }) { Text("Enter") }
        }
    }
}

// ---- Calls (a paired phone's, on a computer) ------------------------------------------------

@Kompozable
fun CallsScreen(app: App) {
    val phone = app.smsPhone()
    val log = app.callLog()
    var number by remember { mutableStateOf("") }
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestCallLog()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint("Aramalar için eşleşmiş bir Android telefon çevrimiçi olmalı.", loading = false)
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = number, onValueChange = { number = it }, singleLine = true, label = "Numara",
                                  modifier = Modifier.weight(1f))
                Spacer(modifier = Modifier.width(8.dp))
                Button(onClick = { app.dial(number) }, enabled = number.isNotBlank()) { Text("Telefondan ara") }
            }
            if (app.callsError().isNotEmpty()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(app.callsError())
                    TextButton(onClick = { app.requestCallLog() }) { Text("Tekrar dene") }
                }
            }
            LazyColumn(modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp, 0.dp, 12.dp, 12.dp),
                       verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (log.isEmpty() && app.callsError().isEmpty()) {
                    item { Hint("Arama kaydı telefondan alınıyor…", loading = true) }
                }
                items(log) { c -> CallRow(c, onCall = { app.dial(c.number) }) }
            }
        }
    }
}

@Kompozable
fun CallRow(c: CallEntry, onCall: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val missed = c.type == "missed" || c.type == "rejected" || c.type == "blocked"
    val kind = when (c.type) {
        "incoming" -> "↙ Gelen"
        "outgoing" -> "↗ Giden"
        "missed" -> "✕ Cevapsız"
        "rejected" -> "✕ Reddedildi"
        "blocked" -> "⊘ Engellendi"
        else -> "Arama"
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(c.title(), style = MaterialTheme.typography.titleSmall, color = if (missed) cs.error else cs.onSurface)
                Text("$kind · ${shortDate(c.date)}${if (c.duration > 0) " · " + duration(c.duration) else ""}",
                     style = MaterialTheme.typography.bodySmall, color = if (missed) cs.error else cs.onSurfaceVariant)
            }
            TextButton(onClick = onCall) { Text("Ara") }
        }
    }
}

// ---- Messages (a paired phone's SMS, on a computer) -----------------------------------------

@Kompozable
fun MessagesScreen(app: App) {
    val phone = app.smsPhone()
    val open = app.smsOpenThread()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestSmsThreads()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint("Mesajlar için eşleşmiş bir Android telefon çevrimiçi olmalı.", loading = false)
        }
    } else if (app.smsError().isNotEmpty()) {
        Column(modifier = Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(app.smsError())
            TextButton(onClick = { app.requestSmsThreads() }) { Text("Tekrar dene") }
        }
    } else if (open != null) {
        Conversation(app, open!!)
    } else {
        ThreadList(app)
    }
}

@Kompozable
fun ThreadList(app: App) {
    val threads = app.smsThreads()
    var composing by remember { mutableStateOf(false) }
    Box(modifier = Modifier.fillMaxSize()) {
        LazyColumn(modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (threads.isEmpty()) {
                item { Hint(if (app.smsLoading()) "Mesajlar telefondan alınıyor…" else "Mesaj yok.", loading = app.smsLoading()) }
            }
            items(threads) { t -> ThreadRow(t, onClick = { app.openSmsThread(t) }) }
        }
        FloatingActionButton(onClick = { composing = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
            Icon(Icons.Filled.Add, contentDescription = "Yeni mesaj")
        }
    }
    if (composing) NewSmsDialog(app, onClose = { composing = false })
}

@Kompozable
fun ThreadRow(t: SmsThread, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(onClick = onClick, modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(40.dp).background(cs.primaryContainer, CircleShape), contentAlignment = Alignment.Center) {
                Text(initial(t.title()), color = cs.onPrimaryContainer, style = MaterialTheme.typography.titleMedium)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(t.title(), style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(t.snippet, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Spacer(modifier = Modifier.width(8.dp))
            Column(horizontalAlignment = Alignment.End) {
                Text(shortDate(t.date), style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant)
                if (t.unread > 0) {
                    Box(modifier = Modifier.padding(top = 4.dp).size(20.dp).background(cs.primary, CircleShape), contentAlignment = Alignment.Center) {
                        Text("${t.unread}", color = cs.onPrimary, style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }
    }
}

@Kompozable
fun Conversation(app: App, thread: SmsThread) {
    val messages = app.smsMessages()
    val list = rememberLazyListState()
    var text by remember { mutableStateOf("") }
    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) list.scrollToItem(messages.size - 1)
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { app.closeSmsThread() }) { Icon(Icons.Filled.ArrowBack, contentDescription = "Geri") }
            Column {
                Text(thread.title(), style = MaterialTheme.typography.titleMedium)
                if (thread.name.isNotEmpty()) Text(thread.address, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.weight(1f).fillMaxWidth(), state = list, contentPadding = PaddingValues(12.dp),
                   verticalArrangement = Arrangement.spacedBy(6.dp)) {
            items(messages) { m -> Bubble(m) }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = text, onValueChange = { text = it }, modifier = Modifier.weight(1f), label = "Mesaj")
            IconButton(onClick = { app.sendSms(thread.address, text); text = "" }, enabled = text.isNotBlank()) {
                Icon(Icons.Filled.Send, contentDescription = "Gönder")
            }
        }
    }
}

@Kompozable
fun Bubble(m: SmsMessage) {
    val cs = MaterialTheme.colorScheme
    val color = if (m.outgoing) cs.primaryContainer else cs.surfaceContainerHigh
    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = if (m.outgoing) Arrangement.End else Arrangement.Start) {
        Column(modifier = Modifier.widthIn(0.dp, 360.dp).background(color, RoundedCornerShape(16.dp)).padding(12.dp, 8.dp)) {
            Text(m.body, style = MaterialTheme.typography.bodyMedium)
            Text(if (m.failed) "${clockTime(m.date)} · gönderilemedi" else if (m.id < 0) "gönderiliyor…" else clockTime(m.date),
                 style = MaterialTheme.typography.labelSmall, color = if (m.failed) cs.error else cs.onSurfaceVariant)
        }
    }
}

@Kompozable
fun NewSmsDialog(app: App, onClose: () -> Unit) {
    var to by remember { mutableStateOf("") }        // what was typed: a name or a number
    var chosen by remember { mutableStateOf("") }    // the contact picked, its name
    var number by remember { mutableStateOf("") }    // the number it goes to
    var body by remember { mutableStateOf("") }
    // Search the phone's contacts as the user types (once typing pauses).
    LaunchedEffect(to) {
        delay(250)
        if (chosen.isEmpty()) app.searchContacts(to)
    }
    val results = app.contactResults()
    AlertDialog(
        onDismissRequest = onClose,
        title = { Text("Yeni mesaj") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = to, onValueChange = { to = it; chosen = ""; number = it }, singleLine = true,
                                  label = "Kişi ya da numara", modifier = Modifier.fillMaxWidth())
                if (chosen.isNotEmpty()) {
                    Text("$chosen · $number", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
                } else {
                    for (c in results.take(5)) {
                        ContactRow(c, onClick = { chosen = c.name; number = c.number; to = c.name })
                    }
                }
                OutlinedTextField(value = body, onValueChange = { body = it }, label = "Mesaj", modifier = Modifier.fillMaxWidth())
            }
        },
        confirmButton = {
            TextButton(onClick = { app.sendSms(number, body); app.searchContacts(""); app.requestSmsThreads(); onClose() },
                       enabled = number.isNotBlank() && body.isNotBlank()) { Text("Gönder") }
        },
        dismissButton = { TextButton(onClick = { app.searchContacts(""); onClose() }) { Text("Vazgeç") } },
    )
}

@Kompozable
fun ContactRow(c: ContactMatch, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Row(modifier = Modifier.fillMaxWidth().clickable(onClick = onClick).padding(4.dp, 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(modifier = Modifier.size(32.dp).background(cs.secondaryContainer, CircleShape), contentAlignment = Alignment.Center) {
            Text(initial(c.name), color = cs.onSecondaryContainer, style = MaterialTheme.typography.labelLarge)
        }
        Spacer(modifier = Modifier.width(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(c.name, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}", style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
        }
    }
}

@Kompozable
fun PhoneNotificationCard(n: PhoneNotification, peerName: String, onDismiss: () -> Unit, onReply: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp)) {
            Text("${n.app} · $peerName · ${clockTime(n.time)}", style = MaterialTheme.typography.labelMedium, color = cs.primary)
            if (n.title.isNotEmpty()) Text(n.title, style = MaterialTheme.typography.titleSmall)
            if (n.text.isNotEmpty()) Text(n.text, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, maxLines = 4, overflow = TextOverflow.Ellipsis)
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (n.canReply) TextButton(onClick = onReply) { Text("Yanıtla") }
                TextButton(onClick = onDismiss) { Text("Kapat") }
            }
        }
    }
}

@Kompozable
fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 16.dp, bottom = 4.dp))
}

@Kompozable
fun Hint(text: String, loading: Boolean) {
    Row(modifier = Modifier.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        if (loading) {
            CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
            Spacer(modifier = Modifier.width(12.dp))
        }
        Text(text, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Kompozable
fun OnlineDot(online: Boolean) {
    val color = if (online) Color(0xFF2E7D32) else MaterialTheme.colorScheme.outline
    Box(modifier = Modifier.size(10.dp).background(color, CircleShape))
}

@Kompozable
fun PairedPeerCard(
    peer: Peer,
    onSendFiles: () -> Unit,
    onDropFiles: (List<String>, List<String>) -> Unit,
    onSendClipboard: () -> Unit,
    onForget: () -> Unit,
    onShowScreen: () -> Unit,
) {
    var dragging by remember { mutableStateOf(false) }
    val cs = MaterialTheme.colorScheme
    val status = when {
        dragging -> "Göndermek için bırakın"
        !peer.online -> "Çevrimdışı"
        isDesktop() -> "Çevrimiçi · dosyaları buraya sürükleyin"
        else -> "Çevrimiçi"
    }
    val drop = Modifier.dragAndDropTarget(
        shouldStartDragAndDrop = { e -> carriesFiles(e) },
        target = DragAndDropTarget(
            onDrop = { e ->
                dragging = false
                onDropFiles(droppedFiles(e), droppedUris(e))
                true
            },
            onEntered = { dragging = true },
            onExited = { dragging = false },
            onEnded = { dragging = false },
        ),
    )
    OutlinedCard(
        modifier = drop.fillMaxWidth(),
        colors = CardDefaults.outlinedCardColors(containerColor = if (dragging) cs.secondaryContainer else cs.surface),
    ) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Box {
                Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(32.dp))
                Box(modifier = Modifier.align(Alignment.TopEnd)) { OnlineDot(peer.online) }
            }
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(peer.name, style = MaterialTheme.typography.titleMedium)
                Text(status, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
            }
            IconButton(onClick = onSendFiles) { Icon(AppIcons.UploadFile, contentDescription = "Dosya gönder") }
            IconButton(onClick = onSendClipboard) { Icon(AppIcons.ContentPaste, contentDescription = "Panoyu gönder") }
            if (isDesktop() && peer.platform == "android" && peer.online) {
                IconButton(onClick = onShowScreen) { Icon(AppIcons.Smartphone, contentDescription = "Ekranı göster") }
            }
            IconButton(onClick = onForget) { Icon(Icons.Filled.Delete, contentDescription = "Cihazı unut") }
        }
    }
}

@Kompozable
fun NearbyPeerCard(peer: Peer, onPair: () -> Unit) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, tint = MaterialTheme.colorScheme.primary)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(peer.name, style = MaterialTheme.typography.titleMedium)
                Text("${peer.platform} · ${shortId(peer.id)}", style = MaterialTheme.typography.bodyMedium,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            FilledTonalButton(onClick = onPair) {
                Icon(AppIcons.Link, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(modifier = Modifier.width(8.dp))
                Text("Eşleş")
            }
        }
    }
}

@Kompozable
fun TransferRow(t: Transfer, peerName: String) {
    val cs = MaterialTheme.colorScheme
    val title = if (t.incoming) "$peerName → bu cihaz" else "Bu cihaz → $peerName"
    val status = when {
        t.error.isNotEmpty() -> "Hata: ${t.error}"
        t.completed -> "Tamamlandı · ${humanSize(t.total)}"
        t.waiting -> "Karşı tarafın onayı bekleniyor… (${humanSize(t.total)})"
        else -> "${humanSize(t.done)} / ${humanSize(t.total)}"
    }
    val icon = when {
        t.error.isNotEmpty() -> AppIcons.Error
        t.completed -> Icons.Filled.CheckCircle
        t.incoming -> AppIcons.Download
        else -> AppIcons.Upload
    }
    val tint = when {
        t.error.isNotEmpty() -> cs.error
        t.completed -> Color(0xFF2E7D32)
        else -> cs.primary
    }
    Card {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, contentDescription = null, tint = tint)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleSmall)
                if (t.active()) {
                    Spacer(modifier = Modifier.height(6.dp))
                    if (t.waiting) {
                        LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
                    } else {
                        LinearProgressIndicator(progress = { t.fraction() }, modifier = Modifier.fillMaxWidth())
                    }
                }
                Spacer(modifier = Modifier.height(4.dp))
                Text(status, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2)
            }
        }
    }
}

@Kompozable
fun PairingCode(code: String, hint: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
        Text(spacedCode(code), style = MaterialTheme.typography.displaySmall)
        Spacer(modifier = Modifier.height(12.dp))
        Text(hint)
    }
}

@Kompozable
fun IncomingPrompt(app: App) {
    val p = app.prompt()
    if (p != null) {
        val prompt = p!!
        if (prompt.isPairing) {
            AlertDialog(
                onDismissRequest = {},
                icon = { Icon(AppIcons.Link, contentDescription = null) },
                title = { Text("${prompt.peerName} eşleşmek istiyor") },
                text = { PairingCode(prompt.code, "Diğer cihazda da aynı kod görünüyorsa onaylayın.") },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text("Kodlar aynı") } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text("Reddet") } },
            )
        } else {
            AlertDialog(
                onDismissRequest = {},
                icon = { Icon(AppIcons.Download, contentDescription = null) },
                title = { Text("${prompt.peerName} dosya göndermek istiyor") },
                text = {
                    Column {
                        Text("${prompt.files.size} dosya, toplam ${humanSize(prompt.totalSize)}")
                        Spacer(modifier = Modifier.height(8.dp))
                        for (f in prompt.files.take(8)) {
                            Text("${f.path}  ·  ${humanSize(f.size)}", style = MaterialTheme.typography.bodySmall,
                                 maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        if (prompt.files.size > 8) Text("… ve ${prompt.files.size - 8} dosya daha", style = MaterialTheme.typography.bodySmall)
                    }
                },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text("Kabul et") } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text("Reddet") } },
            )
        }
    }
}

@Kompozable
fun OutgoingPairingDialog(app: App) {
    val o = app.outgoing()
    if (o != null) {
        val pairing = o!!
        AlertDialog(
            onDismissRequest = {},
            icon = { Icon(AppIcons.Link, contentDescription = null) },
            title = { Text("${pairing.peerName} ile eşleş") },
            text = {
                PairingCode(pairing.code,
                    if (pairing.waitingForPeer) "Diğer cihazın onayı bekleniyor…" else "Bu kod diğer cihazda da görünüyor mu?")
            },
            confirmButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(true) }) { Text("Kodlar aynı") }
            },
            dismissButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(false) }) { Text("İptal") }
            },
        )
    }
}

kompoz::Application kompoz::createApplication(int argc, char** argv) {
    // `airlink --send FILE...`: to the running instance if there is one (this process then exits).
    airlink::platform::handleLaunch(argc, argv);
    // Create the app's states here, outside any composition's snapshot.
    App::get();
    return {.title = "AirLink", .content = [] { AirLinkApp(); }};
}
