// AirLink's UI. Devices: the device list, pairing, transfers, prompts and the phone's notifications.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// Files handed to AirLink from outside (a file manager's "Send with AirLink", Android's share
// sheet): which device gets them.
// Files shared from outside (a file manager's "AirLink ile gönder", the command line): which
// device gets them. On a computer in a small window of its own, so it shows with AirLink's window
// hidden in the tray too; on a phone as a dialog over the app.
@Kompozable
fun ShareDialog(app: App) {
    val files = app.share()
    if (files.isNotEmpty() && app.ready() && !isDesktop()) {
        AlertDialog(
            onDismissRequest = { app.cancelShare() },
            icon = { Icon(AppIcons.Upload, contentDescription = null) },
            title = { Text(shareTitle(files)) },
            text = { ShareChoices(app) },
            confirmButton = { TextButton(onClick = { app.cancelShare() }) { Text(tr("Vazgeç")) } },
        )
    }
}

@Kompozable
fun ShareWindow(app: App) {
    val files = app.share()
    if (files.isNotEmpty() && app.ready()) {
        val state = rememberWindowState(size = DpSize(420.dp, 420.dp))
        Window(onCloseRequest = { app.cancelShare() }, state = state, title = tr("AirLink ile gönder")) {
            Surface(modifier = Modifier.fillMaxSize()) {
                Column(modifier = Modifier.fillMaxSize().padding(20.dp).verticalScroll(rememberScrollState()),
                       verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(AppIcons.Upload, contentDescription = null, tint = MaterialTheme.colorScheme.primary)
                        Spacer(modifier = Modifier.width(12.dp))
                        Text(shareTitle(files), style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f),
                             maxLines = 2, overflow = TextOverflow.Ellipsis)
                    }
                    ShareChoices(app)
                    Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                        TextButton(onClick = { app.cancelShare() }) { Text(tr("Vazgeç")) }
                    }
                }
            }
        }
    }
}

@Kompozable
fun ShareChoices(app: App) {
    val paired = app.pairedPeers()
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        if (paired.isEmpty()) {
            Text(tr("Henüz eşleşmiş cihaz yok. Önce bir cihazla eşleşin."))
        } else {
            Text(tr("Hangi cihaza?"), color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (app.canFling() && app.flingEnabled() && app.flingTarget().isNotEmpty()) {
                Text(trf("Ya da telefonu {} cihazına doğru fırlatın.", app.peerName(app.flingTarget())),
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
            }
            for (peer in paired) {
                OutlinedButton(onClick = { app.shareTo(peer.id) }, modifier = Modifier.fillMaxWidth(), enabled = peer.online) {
                    Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(modifier = Modifier.width(8.dp))
                    Text(if (peer.online) peer.name else trf("{} (çevrimdışı)", peer.name), modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Kompozable
fun DeviceList(app: App, clipboard: ClipboardManager, onOpenSettings: () -> Unit) {
    val transfers = app.transfers()
    val paired = app.pairedPeers()
    val nearby = app.nearbyPeers()
    val notifications = app.phoneNotifications()
    var forgetting by remember { mutableStateOf("") }
    var replyTo by remember { mutableStateOf(PhoneNotification()) }
    var replyText by remember { mutableStateOf("") }
    var manualAddress by remember { mutableStateOf("") }
    var pairByAddress by remember { mutableStateOf(false) }
    var allTransfers by remember { mutableStateOf(false) }

    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        val missing = missingPermissions(app)
        if (missing > 0) {
            item { MissingPermissionsBanner(missing, onOpenSettings) }
        }

        item { SectionTitle(tr("Eşleşmiş cihazlar")) }
        if (paired.isEmpty()) {
            item { NoPairedDevices() }
        } else {
            items(paired) { peer ->
                PairedPeerCard(
                    peer = peer,
                    onSendFiles = { app.pickAndSend(peer.id) },
                    onDropFiles = { paths, uris -> app.sendDropped(peer.id, paths, uris) },
                    onSendClipboard = { app.sendClipboard(peer.id, clipboard.getText()) },
                    onForget = { forgetting = peer.id },
                    onShowScreen = { app.startScreen(peer.id) },
                    onControl = { app.startDesktop(peer.id) },
                    onTrust = { app.setTrusted(peer.id, it) },
                    onRename = { app.renamePeer(peer.id, it) },
                    battery = app.phoneBattery(peer.id),
                    onFind = { app.findPhone(peer.id, it) },
                )
            }
        }

        // The running ones and the latest few finished ones; the rest on request.
        if (transfers.isNotEmpty()) {
            val shown = if (allTransfers) transfers else recentTransfers(transfers, 3)
            item { SectionTitle(tr("Aktarımlar")) }
            items(shown) { t -> TransferRow(t, app.peerName(t.peerId), onCancel = { app.cancelTransfer(t.id) }) }
            if (shown.size < transfers.size) {
                item {
                    TextButton(onClick = { allTransfers = true }) { Text(trf("Tümünü göster ({})", transfers.size)) }
                }
            }
        }

        if (notifications.isNotEmpty()) {
            item { SectionTitle(tr("Telefon bildirimleri")) }
            items(notifications) { n ->
                PhoneNotificationCard(n, app.peerName(n.peerId),
                    onDismiss = { app.dismissPhoneNotification(n) },
                    onReply = { replyTo = n })
            }
        }

        item { SectionTitle(tr("Yakındaki cihazlar")) }
        if (nearby.isEmpty()) {
            item { Hint(tr("Aranıyor… Diğer cihazda AirLink açık ve aynı ağda olmalı."), loading = true) }
        } else {
            items(nearby) { peer -> NearbyPeerCard(peer, onPair = { app.pair(peer.id) }) }
        }
        item {
            if (pairByAddress) {
                OutlinedCard(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(tr("IP adresiyle eşleştir"), style = MaterialTheme.typography.titleSmall)
                        Text(tr("Ağ keşfi çalışmıyorsa diğer cihazın IP adresini yazın. Varsayılan bağlantı noktası 47820."),
                             style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = manualAddress, onValueChange = { manualAddress = it }, singleLine = true,
                                              label = tr("IP adresi veya IP:port"), modifier = Modifier.weight(1f))
                            Spacer(modifier = Modifier.width(8.dp))
                            Button(onClick = { app.pairAddress(manualAddress) }, enabled = manualAddress.isNotBlank()) { Text(tr("Eşleştir")) }
                        }
                    }
                }
            } else {
                TextButton(onClick = { pairByAddress = true }) { Text(tr("Cihaz görünmüyor mu? IP adresiyle eşleştir")) }
            }
        }
    }

    if (replyTo.key.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { replyTo = PhoneNotification(); replyText = "" },
            title = { Text(trf("{} yanıtla", replyTo.title)) },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(replyTo.text, style = MaterialTheme.typography.bodySmall, maxLines = 3, overflow = TextOverflow.Ellipsis)
                    OutlinedTextField(value = replyText, onValueChange = { replyText = it }, modifier = Modifier.fillMaxWidth(), label = tr("Yanıt"))
                }
            },
            confirmButton = {
                TextButton(onClick = { app.replyPhoneNotification(replyTo, replyText); replyTo = PhoneNotification(); replyText = "" }) { Text(tr("Gönder")) }
            },
            dismissButton = { TextButton(onClick = { replyTo = PhoneNotification(); replyText = "" }) { Text(tr("Vazgeç")) } },
        )
    }

    if (forgetting.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { forgetting = "" },
            title = { Text(trf("{} unutulsun mu?", app.peerName(forgetting))) },
            text = { Text(tr("Tekrar dosya paylaşmak için yeniden eşleşmeniz gerekecek.")) },
            confirmButton = { TextButton(onClick = { app.forget(forgetting); forgetting = "" }) { Text(tr("Unut")) } },
            dismissButton = { TextButton(onClick = { forgetting = "" }) { Text(tr("Vazgeç")) } },
        )
    }
}

// Permissions this device still needs for what it can share (the phone's, mostly).
@Kompozable
fun missingPermissions(app: App): Int {
    var n = 0
    if (app.canShareNotifications() && !app.notificationAccess()) n++
    if (app.canShareSms() && !app.smsAccess()) n++
    if (app.canShareCalls() && !app.callAccess()) n++
    if (app.canShareScreen() && !app.screenControl()) n++
    return n
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
                if (n.canReply) TextButton(onClick = onReply) { Text(tr("Yanıtla")) }
                TextButton(onClick = onDismiss) { Text(tr("Kapat")) }
            }
        }
    }
}

@Kompozable
fun PairedPeerCard(
    peer: Peer,
    onSendFiles: () -> Unit,
    onDropFiles: (List<String>, List<String>) -> Unit,
    onSendClipboard: () -> Unit,
    onForget: () -> Unit,
    onShowScreen: () -> Unit,
    onControl: () -> Unit,
    onTrust: (Boolean) -> Unit,
    onRename: (String) -> Unit,
    battery: PhoneBattery,
    onFind: (Boolean) -> Unit,
) {
    var dragging by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var newName by remember { mutableStateOf("") }
    val nameFocus = remember { FocusRequester() }
    LaunchedEffect(renaming) { if (renaming) nameFocus.requestFocus() }
    val cs = MaterialTheme.colorScheme
    val status = when {
        dragging -> tr("Göndermek için bırakın")
        !peer.online -> if (peer.autoAccept) tr("Çevrimdışı · Güvenilir") else tr("Çevrimdışı")
        peer.autoAccept -> tr("Çevrimiçi · Güvenilir")
        isDesktop() -> tr("Çevrimiçi · dosyaları buraya sürükleyin")
        else -> tr("Çevrimiçi")
    }
    var confirmTrust by remember { mutableStateOf(false) }
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
    var menu by remember { mutableStateOf(false) }
    OutlinedCard(
        modifier = drop.fillMaxWidth(),
        colors = CardDefaults.outlinedCardColors(containerColor = if (dragging) cs.secondaryContainer else cs.surface),
    ) {
        Column(modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, end = 4.dp, bottom = 12.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box {
                    Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(32.dp))
                    Box(modifier = Modifier.align(Alignment.TopEnd)) { OnlineDot(peer.online) }
                }
                Spacer(modifier = Modifier.width(16.dp))
                Column(modifier = Modifier.weight(1f)) {
                    // The name, and its pencil: a field in its place to rename it (✔ keeps, × leaves it).
                    if (renaming) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            // (compact: the name's own size, in a thin outline)
                            BasicTextField(value = newName, onValueChange = { newName = it }, singleLine = true, maxLines = 1,
                                           textStyle = TextStyle(font = MaterialTheme.typography.titleMedium, color = cs.onSurface),
                                           modifier = Modifier.weight(1f, false).widthIn(120.dp, 280.dp).focusRequester(nameFocus),
                                           onSubmit = {
                                               if (newName.isNotBlank()) onRename(newName.trim())
                                               renaming = false
                                           },
                                           decorator = { innerTextField ->
                                               Box(modifier = Modifier.border(1.dp, cs.primary, RoundedCornerShape(6.dp)).padding(8.dp, 3.dp)) {
                                                   innerTextField()
                                               }
                                           })
                            IconButton(onClick = { renaming = false }, modifier = Modifier.size(32.dp)) {
                                Icon(Icons.Filled.Close, contentDescription = tr("Vazgeç"), modifier = Modifier.size(18.dp))
                            }
                            IconButton(onClick = {
                                onRename(newName.trim())
                                renaming = false
                            }, enabled = newName.isNotBlank(), modifier = Modifier.size(32.dp)) {
                                Icon(Icons.Filled.Check, contentDescription = tr("Kaydet"), tint = cs.primary, modifier = Modifier.size(18.dp))
                            }
                        }
                    } else {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(peer.name, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis,
                                 modifier = Modifier.weight(1f, false))
                            IconButton(onClick = {
                                newName = peer.name
                                renaming = true
                            }, modifier = Modifier.size(32.dp)) {
                                Icon(Icons.Filled.Edit, contentDescription = tr("Yeniden adlandır"), modifier = Modifier.size(16.dp), tint = cs.onSurfaceVariant)
                            }
                        }
                    }
                    if (peer.name != peer.deviceName && peer.deviceName.isNotEmpty() && !renaming) {
                        Text(trf("Kendi adı: {}", peer.deviceName), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant,
                             maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    Text(status, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
                    if (peer.online && battery.level >= 0) BatteryLine(battery)
                }
                IconButton(onClick = { if (peer.autoAccept) onTrust(false) else confirmTrust = true }) {
                    if (peer.autoAccept) Icon(AppIcons.VerifiedUser, contentDescription = tr("Güvenilir: geri al"), tint = cs.primary)
                    else Icon(AppIcons.Shield, contentDescription = tr("Güvenilir yap"), tint = cs.onSurfaceVariant)
                }
                Box {
                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = tr("Diğer")) }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        DropdownMenuItem(
                            text = { Text(tr("Cihazı unut")) },
                            onClick = {
                                menu = false
                                onForget()
                            },
                            leadingIcon = { Icon(Icons.Filled.Delete, contentDescription = null) })
                    }
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(start = 48.dp, end = 12.dp)) {
                AssistChip(onClick = onSendFiles, enabled = peer.online, label = { Text(tr("Dosya gönder")) },
                           leadingIcon = { Icon(AppIcons.UploadFile, contentDescription = null, modifier = Modifier.size(18.dp)) })
                AssistChip(onClick = onSendClipboard, enabled = peer.online, label = { Text(tr("Pano")) },
                           leadingIcon = { Icon(AppIcons.ContentPaste, contentDescription = null, modifier = Modifier.size(18.dp)) })
                if (isDesktop() && peer.platform == "android") {
                    AssistChip(onClick = onShowScreen, enabled = peer.online, label = { Text(tr("Ekranı göster")) },
                               leadingIcon = { Icon(AppIcons.Smartphone, contentDescription = null, modifier = Modifier.size(18.dp)) })
                    AssistChip(onClick = { onFind(!battery.ringing) }, enabled = peer.online,
                               label = { Text(if (battery.ringing) tr("Çalmayı durdur") else tr("Telefonu bul")) },
                               leadingIcon = { Icon(AppIcons.PhonelinkRing, contentDescription = null, modifier = Modifier.size(18.dp)) })
                }
                if (!isDesktop() && peer.platform != "android") {
                    AssistChip(onClick = onControl, enabled = peer.online, label = { Text(tr("Kontrol et")) },
                               leadingIcon = { Icon(AppIcons.Computer, contentDescription = null, modifier = Modifier.size(18.dp)) })
                }
            }
        }
    }
    if (confirmTrust) {
        AlertDialog(
            onDismissRequest = { confirmTrust = false },
            icon = { Icon(AppIcons.VerifiedUser, contentDescription = null) },
            title = { Text(trf("{} güvenilir olsun mu?", peer.name)) },
            text = {
                Text(tr("Bu cihazdan gelen dosyalar sorulmadan kaydedilir, bilgisayarı kontrol etme istekleri sorulmadan kabul edilir ve kopyaladığınız metinler ona otomatik gider. Kalkan simgesine yeniden dokunarak istediğiniz zaman geri alabilirsiniz."))
            },
            confirmButton = { TextButton(onClick = { confirmTrust = false; onTrust(true) }) { Text(tr("Güvenilir yap")) } },
            dismissButton = { TextButton(onClick = { confirmTrust = false }) { Text(tr("Vazgeç")) } },
        )
    }
}

fun platformName(platform: String): String = when (platform) {
    "android" -> "Android"
    "linux" -> "Linux"
    "windows" -> "Windows"
    "macos" -> "macOS"
    else -> platform
}

// No paired device yet: what pairing gives and how to start.
@Kompozable
fun NoPairedDevices() {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.surfaceVariant), modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.Devices, contentDescription = null, modifier = Modifier.size(36.dp), tint = cs.primary)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(tr("Henüz eşleşmiş cihaz yok"), style = MaterialTheme.typography.titleSmall)
                Text(tr("Diğer cihazda da AirLink'i açın; aşağıda görününce Eşleş'e basıp iki ekrandaki kodu karşılaştırın."),
                     style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
            }
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
                Text("${platformName(peer.platform)} · ${shortId(peer.id)}", style = MaterialTheme.typography.bodyMedium,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            FilledTonalButton(onClick = onPair) {
                Icon(AppIcons.Link, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(modifier = Modifier.width(8.dp))
                Text(tr("Eşleş"))
            }
        }
    }
}

@Kompozable
fun TransferRow(t: Transfer, peerName: String, onCancel: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val title = if (t.incoming) trf("{} → bu cihaz", peerName) else trf("Bu cihaz → {}", peerName)
    val status = when {
        t.error == tr("İptal edildi") || t.error == tr("Karşı cihaz iptal etti") -> t.error
        t.error.isNotEmpty() -> trf("Hata: {}", t.error)
        t.completed -> trf("Tamamlandı · {}", humanSize(t.total))
        t.waiting -> trf("Karşı tarafın onayı bekleniyor… ({})", humanSize(t.total))
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
            if (t.active()) {
                Spacer(modifier = Modifier.width(8.dp))
                IconButton(onClick = onCancel) { Icon(Icons.Filled.Close, contentDescription = tr("Aktarımı iptal et")) }
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
                title = { Text(trf("{} eşleşmek istiyor", prompt.peerName)) },
                text = { PairingCode(prompt.code, tr("Diğer cihazda da aynı kod görünüyorsa onaylayın.")) },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text(tr("Kodlar aynı")) } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text(tr("Reddet")) } },
            )
        } else {
            AlertDialog(
                onDismissRequest = {},
                icon = { Icon(AppIcons.Download, contentDescription = null) },
                title = { Text(trf("{} dosya göndermek istiyor", prompt.peerName)) },
                text = {
                    Column {
                        Text(trf("{} dosya, toplam {}", prompt.files.size, humanSize(prompt.totalSize)))
                        Spacer(modifier = Modifier.height(8.dp))
                        for (f in prompt.files.take(8)) {
                            Text("${f.path}  ·  ${humanSize(f.size)}", style = MaterialTheme.typography.bodySmall,
                                 maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        if (prompt.files.size > 8) Text(trf("… ve {} dosya daha", prompt.files.size - 8), style = MaterialTheme.typography.bodySmall)
                    }
                },
                confirmButton = { TextButton(onClick = { app.respond(prompt.requestId, true) }) { Text(tr("Kabul et")) } },
                dismissButton = { TextButton(onClick = { app.respond(prompt.requestId, false) }) { Text(tr("Reddet")) } },
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
            title = { Text(trf("{} ile eşleş", pairing.peerName)) },
            text = {
                PairingCode(pairing.code,
                    if (pairing.waitingForPeer) tr("Diğer cihazın onayı bekleniyor…") else tr("Bu kod diğer cihazda da görünüyor mu?"))
            },
            confirmButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(true) }) { Text(tr("Kodlar aynı")) }
            },
            dismissButton = {
                if (!pairing.waitingForPeer) TextButton(onClick = { app.confirmOutgoing(false) }) { Text(tr("İptal")) }
            },
        )
    }
}
