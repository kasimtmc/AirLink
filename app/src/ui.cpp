// AirLink's UI. The @Kompozable functions are Kotlin, turned into C++ by kompozc; the rest is C++.

#include <kompoz/kompoz.hpp>

#include "app_icons.hpp"
#include "app_state.hpp"
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
        Box(modifier = Modifier.fillMaxSize().padding(padding)) {
            if (app.ready()) {
                DeviceList(app, clipboard)
            } else if (app.startError().isNotEmpty()) {
                Text("Başlatılamadı:\n${app.startError()}", modifier = Modifier.align(Alignment.Center).padding(24.dp))
            } else {
                CircularProgressIndicator(modifier = Modifier.align(Alignment.Center))
            }
        }
    }

    IncomingPrompt(app)
    OutgoingPairingDialog(app)
    ShareDialog(app)
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
    var forgetting by remember { mutableStateOf("") }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { ThisDeviceCard(app.me()) }

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
