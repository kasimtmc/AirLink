// AirLink's UI. The tray panel: devices, the phone's media, messages, notifications and calls at a glance.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// What AirLink offers without its window: the devices and their quick actions, the phone's media,
// messages, notifications, calls and contacts.
@Kompozable
fun TrayPanel(app: App, onOpenApp: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val clip = clipboard()
    val text = clipboardManager()
    val online = app.pairedPeers().filter { it.online }
    val phone = online.any { it.platform == "android" }
    var copied by remember { mutableStateOf(copiedFiles(clip)) }
    var query by remember { mutableStateOf("") }
    var replyThread by remember { mutableStateOf("") }
    var replyText by remember { mutableStateOf("") }
    // Ctrl+V sends what is copied (files, else text) to the chosen device: the panel closes when
    // anything else is clicked, so dragging onto it can't work.
    var chosen by remember { mutableStateOf("") }
    var pasted by remember { mutableStateOf("") }
    val target = if (online.any { it.id == chosen }) chosen
                 else if (online.any { it.platform == "android" }) online.first { it.platform == "android" }.id
                 else if (online.isNotEmpty()) online.first().id else ""
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    // Passwords in the clipboard history go as the panel closes.
    DisposableEffect(Unit) {
        onDispose { app.dropPasswords() }
    }
    LaunchedEffect(phone) {
        if (phone) {
            app.requestPhoneMedia()
            app.requestSmsThreads()
            app.requestCallLog()
        }
    }
    LaunchedEffect(Unit) {
        while (true) {
            copied = copiedFiles(clip)
            delay(1500)
        }
    }
    LazyColumn(modifier = Modifier.fillMaxSize().background(cs.surfaceContainer)
                   .onKeyEvent { e ->
                       if (e.type != KeyEventType.KeyDown || !e.isCtrlPressed || e.isAltPressed || e.key != Key.V) false
                       else if (target.isEmpty()) {
                           pasted = tr("Gönderilecek çevrimiçi cihaz yok")
                           true
                       } else {
                           val files = copiedFiles(clip)
                           val copiedText = text.getText()
                           if (files.isNotEmpty()) {
                               app.sendFiles(target, files)
                               pasted = if (files.size == 1) trf("1 dosya {} cihazına gönderildi", app.peerName(target))
                                        else trf("{} dosya {} cihazına gönderildi", files.size, app.peerName(target))
                           } else if (copiedText.isNotEmpty()) {
                               app.sendClipboard(target, copiedText)
                               pasted = trf("Metin {} cihazının panosuna gönderildi", app.peerName(target))
                           } else {
                               pasted = tr("Panoda gönderilecek dosya ya da metin yok")
                           }
                           true
                       }
                   }
                   .focusRequester(focus)
                   .focusable(),
               contentPadding = PaddingValues(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        item {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("AirLink", style = MaterialTheme.typography.titleMedium)
                    Text(if (online.isEmpty()) tr("Bağlı cihaz yok") else trf("{} cihaz bağlı", online.size),
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                }
                TextButton(onClick = onOpenApp) { Text(tr("AirLink'i aç")) }
                TextButton(onClick = { exitApplication() }) { Text(tr("Çık")) }
            }
        }
        if (target.isNotEmpty()) {
            item {
                Text(if (pasted.isNotEmpty()) pasted
                     else trf("Ctrl+V: kopyaladığınız dosyalar ya da metin {} cihazına gider", app.peerName(target)),
                     style = MaterialTheme.typography.bodySmall, color = if (pasted.isNotEmpty()) cs.primary else cs.onSurfaceVariant,
                     modifier = Modifier.padding(horizontal = 4.dp))
            }
        }
        // Devices
        if (online.isEmpty()) {
            item {
                Text(tr("Eşleşmiş cihazlarınızdan biri çevrimiçi olunca burada görünür."), style = MaterialTheme.typography.bodySmall,
                     color = cs.onSurfaceVariant, modifier = Modifier.padding(4.dp))
            }
        }
        for (peer in online) {
            item { TrayDevice(app, peer, copied, text, peer.id == target && online.size > 1, onChoose = { chosen = peer.id }) }
        }
        // Media
        val media = app.phoneMedia()
        if (media.active && phone) {
            item { TrayMedia(app, media) }
        }
        // Messages
        val threads = app.smsThreads()
        if (phone && threads.isNotEmpty()) {
            item { TraySection(tr("Son mesajlar")) }
            items(threads.take(3)) { t ->
                Column(modifier = Modifier.fillMaxWidth()) {
                    Row(modifier = Modifier.fillMaxWidth().clickable(onClick = {
                            replyThread = if (replyThread == t.address) "" else t.address
                            replyText = ""
                        }).padding(4.dp, 4.dp), verticalAlignment = Alignment.CenterVertically) {
                        CallAvatar(t.title(), 32)
                        Spacer(modifier = Modifier.width(10.dp))
                        Column(modifier = Modifier.weight(1f)) {
                            Text(t.title(), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(t.snippet, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 1,
                                 overflow = TextOverflow.Ellipsis)
                        }
                        Text(clockTime(t.date), style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant)
                    }
                    if (replyThread == t.address) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = replyText, onValueChange = { replyText = it }, singleLine = true, label = tr("Yanıt"),
                                              modifier = Modifier.weight(1f))
                            IconButton(onClick = {
                                    app.sendSms(t.address, replyText)
                                    replyThread = ""
                                    replyText = ""
                                }, enabled = replyText.isNotBlank()) { Icon(Icons.Filled.Send, contentDescription = tr("Gönder")) }
                        }
                    }
                }
            }
        }
        // Notifications
        val notifications = app.phoneNotifications()
        if (notifications.isNotEmpty()) {
            item { TraySection(tr("Bildirimler")) }
            items(notifications.take(4)) { n ->
                Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("${n.app} · ${clockTime(n.time)}", style = MaterialTheme.typography.labelSmall, color = cs.primary)
                        Text(if (n.title.isEmpty()) n.text else n.title, style = MaterialTheme.typography.labelLarge, maxLines = 1,
                             overflow = TextOverflow.Ellipsis)
                        if (n.title.isNotEmpty() && n.text.isNotEmpty()) {
                            Text(n.text, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2,
                                 overflow = TextOverflow.Ellipsis)
                        }
                    }
                    IconButton(onClick = { app.dismissPhoneNotification(n) }) { Icon(Icons.Filled.Close, contentDescription = tr("Kapat")) }
                }
            }
        }
        // Calls
        val calls = app.callLog()
        if (phone && calls.isNotEmpty()) {
            item { TraySection(tr("Son çağrılar")) }
            items(calls.take(4)) { c ->
                Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                    CallAvatar(c.title(), 32)
                    Spacer(modifier = Modifier.width(10.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(c.title(), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis,
                             color = if (c.type == "missed") cs.error else cs.onSurface)
                        Text("${callTypeLabel(c.type)} · ${clockTime(c.date)}", style = MaterialTheme.typography.bodySmall,
                             color = cs.onSurfaceVariant)
                    }
                    IconButton(onClick = { app.dial(c.number) }) { Icon(AppIcons.Phone, contentDescription = tr("Ara")) }
                }
            }
        }
        // Contacts
        if (phone) {
            item { TraySection(tr("Kişiler")) }
            item {
                OutlinedTextField(value = query, onValueChange = {
                        query = it
                        if (it.length >= 2) app.searchContacts(it)
                    }, singleLine = true, label = tr("Kişi ya da numara ara"), modifier = Modifier.fillMaxWidth())
            }
            if (query.length >= 2) {
                items(app.contactResults().take(5)) { c ->
                    Row(modifier = Modifier.fillMaxWidth().padding(4.dp, 2.dp), verticalAlignment = Alignment.CenterVertically) {
                        CallAvatar(c.name, 32)
                        Spacer(modifier = Modifier.width(10.dp))
                        Column(modifier = Modifier.weight(1f)) {
                            Text(c.name, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                            Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}", style = MaterialTheme.typography.bodySmall,
                                 color = cs.onSurfaceVariant)
                        }
                        IconButton(onClick = { app.dial(c.number) }) { Icon(AppIcons.Phone, contentDescription = tr("Ara")) }
                        IconButton(onClick = { replyThread = c.number; replyText = "" }) { Icon(AppIcons.Sms, contentDescription = tr("Mesaj")) }
                    }
                    if (replyThread == c.number) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            OutlinedTextField(value = replyText, onValueChange = { replyText = it }, singleLine = true, label = tr("Mesaj"),
                                              modifier = Modifier.weight(1f))
                            IconButton(onClick = {
                                    app.sendSms(c.number, replyText)
                                    replyThread = ""
                                    replyText = ""
                                }, enabled = replyText.isNotBlank()) { Icon(Icons.Filled.Send, contentDescription = tr("Gönder")) }
                        }
                    }
                }
                if (query.any { it.isDigit() }) {
                    item {
                        TextButton(onClick = { app.dial(query) }) { Text(trf("{} numarasını ara", query)) }
                    }
                }
            }
        }
    }
}

@Preview(widthDp = 380, heightDp = 560)
@Kompozable
fun TrayPanelPreview() {
    MaterialTheme { TrayPanel(appState(), onOpenApp = {}) }
}

@Kompozable
fun TraySection(title: String) {
    Text(title, style = MaterialTheme.typography.labelLarge, color = MaterialTheme.colorScheme.primary,
         modifier = Modifier.padding(start = 4.dp, top = 10.dp, bottom = 2.dp))
}

// A connected device in the panel: its actions, and a place to drop files on.
@Kompozable
fun TrayDevice(app: App, peer: Peer, copied: List<String>, text: ClipboardManager, chosen: Boolean, onChoose: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var dragging by remember { mutableStateOf(false) }
    val drop = Modifier.dragAndDropTarget(
        shouldStartDragAndDrop = { e -> carriesFiles(e) },
        target = DragAndDropTarget(
            onDrop = { e ->
                dragging = false
                app.sendDropped(peer.id, droppedFiles(e), droppedUris(e))
                true
            },
            onEntered = { dragging = true },
            onExited = { dragging = false },
            onEnded = { dragging = false },
        ),
    )
    // With more than one device, a click on a card makes it the one Ctrl+V sends to (outlined).
    Card(modifier = drop.fillMaxWidth().clickable(onClick = onChoose)
                        .then(if (chosen) Modifier.border(2.dp, cs.primary, RoundedCornerShape(12.dp)) else Modifier),
         colors = CardDefaults.cardColors(containerColor = if (dragging) cs.secondaryContainer else cs.surface)) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box {
                    Icon(AppIcons.forPlatform(peer.platform), contentDescription = null, modifier = Modifier.size(28.dp))
                    Box(modifier = Modifier.align(Alignment.TopEnd)) { OnlineDot(true) }
                }
                Spacer(modifier = Modifier.width(12.dp))
                val battery = app.phoneBattery(peer.id)
                Column(modifier = Modifier.weight(1f)) {
                    Text(peer.name, style = MaterialTheme.typography.titleSmall)
                    if (battery.level >= 0 && !dragging) BatteryLine(battery)
                    Text(if (dragging) tr("Göndermek için bırakın") else if (chosen) tr("Ctrl+V buraya gönderir") else tr("Dosyaları buraya sürükleyin"),
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                }
                if (peer.platform == "android") {
                    IconButton(onClick = { app.findPhone(peer.id, !battery.ringing) }) {
                        Icon(AppIcons.PhonelinkRing, contentDescription = if (battery.ringing) tr("Çalmayı durdur") else tr("Telefonu bul"),
                             tint = if (battery.ringing) cs.primary else cs.onSurfaceVariant)
                    }
                    IconButton(onClick = {
                            app.startScreen(peer.id)
                            setAppWindowVisible(true)
                        }) { Icon(AppIcons.Smartphone, contentDescription = tr("Ekranı göster")) }
                }
            }
            FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.padding(start = 40.dp)) {
                AssistChip(onClick = { app.pickAndSend(peer.id) }, label = { Text(tr("Dosya")) },
                           leadingIcon = { Icon(AppIcons.UploadFile, contentDescription = null, modifier = Modifier.size(16.dp)) })
                AssistChip(onClick = { app.sendClipboard(peer.id, text.getText()) }, label = { Text(tr("Pano")) },
                           leadingIcon = { Icon(AppIcons.ContentPaste, contentDescription = null, modifier = Modifier.size(16.dp)) })
                if (copied.isNotEmpty()) {
                    AssistChip(onClick = { app.sendFiles(peer.id, copied) },
                               label = { Text(if (copied.size == 1) tr("Kopyalanan dosya") else trf("Kopyalanan {} dosya", copied.size)) },
                               leadingIcon = { Icon(AppIcons.Upload, contentDescription = null, modifier = Modifier.size(16.dp)) })
                }
            }
        }
    }
}

// What plays on the phone: its cover, title and the transport buttons.
@Kompozable
fun TrayMedia(app: App, media: PhoneMedia) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (media.art.width > 0) {
                Image(media.art, contentDescription = null, modifier = Modifier.size(56.dp).clip(RoundedCornerShape(8.dp)))
            } else {
                Box(modifier = Modifier.size(56.dp).background(cs.primaryContainer, RoundedCornerShape(8.dp)), contentAlignment = Alignment.Center) {
                    Icon(AppIcons.VolumeUp, contentDescription = null, tint = cs.onPrimaryContainer)
                }
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(media.title, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(if (media.artist.isEmpty()) media.app else "${media.artist} · ${media.app}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            IconButton(onClick = { app.phoneMediaControl("previous") }, enabled = media.canPrevious) {
                Icon(Icons.Filled.KeyboardArrowLeft, contentDescription = tr("Önceki"))
            }
            IconButton(onClick = { app.phoneMediaControl("toggle") }) {
                Icon(if (media.playing) Icons.Filled.Pause else Icons.Filled.PlayArrow, contentDescription = if (media.playing) tr("Duraklat") else tr("Çal"))
            }
            IconButton(onClick = { app.phoneMediaControl("next") }, enabled = media.canNext) {
                Icon(Icons.Filled.KeyboardArrowRight, contentDescription = tr("Sonraki"))
            }
        }
    }
}
