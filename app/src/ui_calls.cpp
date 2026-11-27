// AirLink's UI. Calls: the call bar and screen, the incoming call, the phone's call log, contacts and dialer.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// A paired phone's call in progress: who, for how long, and its controls.
@Kompozable
fun ActiveCallBar(app: App, call: ActiveCall, onOpen: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var now by remember { mutableStateOf(nowMs()) }
    LaunchedEffect(call.since) {
        while (true) {
            now = nowMs()
            delay(1000)
        }
    }
    Card(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp).clickable(onClick = onOpen),
         colors = CardDefaults.cardColors(containerColor = cs.tertiaryContainer)) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), verticalAlignment = Alignment.CenterVertically) {
            CallAvatar(call.title(), 40)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(call.title(), style = MaterialTheme.typography.titleMedium, color = cs.onTertiaryContainer,
                     maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text("${callStateLabel(call)} · ${elapsed(call.since, now)}", style = MaterialTheme.typography.bodySmall,
                     color = cs.onTertiaryContainer)
            }
            IconButton(onClick = { app.callControl("call.mute", if (call.muted) "{\"on\":false}" else "{\"on\":true}") }) {
                Icon(if (call.muted) AppIcons.MicOff else AppIcons.Mic, contentDescription = if (call.muted) tr("Mikrofonu aç") else tr("Sessize al"),
                     tint = cs.onTertiaryContainer)
            }
            TextButton(onClick = onOpen) { Text(tr("Aç")) }
            Box(modifier = Modifier.size(40.dp).background(cs.error, CircleShape).clickable(onClick = { app.callControl("call.hangup", "{}") }),
                contentAlignment = Alignment.Center) {
                Icon(AppIcons.CallEnd, contentDescription = tr("Aramayı kapat"), tint = cs.onError, modifier = Modifier.size(20.dp))
            }
        }
    }
}

// A round control of the call screen, its label under it (as a phone's dialer has them).
@Kompozable
fun RoundAction(icon: ImageVector, label: String, selected: Boolean, enabled: Boolean, onClick: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    val container = if (selected) cs.primary else cs.surfaceVariant
    val content = if (!enabled) cs.onSurface.copy(alpha = 0.38f) else if (selected) cs.onPrimary else cs.onSurfaceVariant
    Column(modifier = Modifier.width(96.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(modifier = (if (enabled) Modifier.clickable(onClick = onClick) else Modifier).size(64.dp)
                           .background(if (enabled) container else cs.surfaceVariant.copy(alpha = 0.5f), CircleShape),
            contentAlignment = Alignment.Center) {
            Icon(icon, contentDescription = label, tint = content)
        }
        Spacer(modifier = Modifier.height(6.dp))
        Text(label, style = MaterialTheme.typography.labelMedium, textAlign = TextAlign.Center,
             color = if (enabled) cs.onSurface else cs.onSurface.copy(alpha = 0.38f), maxLines = 2)
    }
}

@Kompozable
fun CallAvatar(title: String, size: Int) {
    val cs = MaterialTheme.colorScheme
    val letters = initials(title)
    Box(modifier = Modifier.size(size.dp).background(cs.primaryContainer, CircleShape), contentAlignment = Alignment.Center) {
        if (letters.isEmpty()) Icon(Icons.Filled.Person, contentDescription = null, tint = cs.onPrimaryContainer,
                                    modifier = Modifier.size((size / 2).dp))
        else Text(letters, color = cs.onPrimaryContainer,
                  style = if (size >= 80) MaterialTheme.typography.displaySmall else MaterialTheme.typography.titleMedium)
    }
}

// The call in progress, as a phone's in-call screen: who, how long, the controls, hang up.
@Kompozable
fun CallScreen(app: App, call: ActiveCall, onMinimize: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    var now by remember { mutableStateOf(nowMs()) }
    var keypad by remember { mutableStateOf(false) }
    var typed by remember { mutableStateOf("") }
    var routes by remember { mutableStateOf(false) }
    LaunchedEffect(call.since) {
        while (true) {
            now = nowMs()
            delay(1000)
        }
    }
    Column(modifier = Modifier.fillMaxSize().background(cs.surfaceContainer), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onMinimize) { Icon(Icons.Filled.KeyboardArrowDown, contentDescription = tr("Küçült")) }
            Text(tr("Küçült"), style = MaterialTheme.typography.labelLarge, modifier = Modifier.weight(1f))
            Text(app.peerName(call.peerId), style = MaterialTheme.typography.labelLarge, color = cs.onSurfaceVariant,
                 modifier = Modifier.padding(end = 12.dp))
        }
        Column(modifier = Modifier.weight(1f).widthIn(0.dp, 420.dp).padding(horizontal = 24.dp),
               horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            CallAvatar(call.title(), 112)
            Spacer(modifier = Modifier.height(20.dp))
            Text(call.title(), style = MaterialTheme.typography.headlineMedium, textAlign = TextAlign.Center, maxLines = 2)
            if (call.name.isNotEmpty() && call.number.isNotEmpty()) {
                Text(call.number, style = MaterialTheme.typography.bodyLarge, color = cs.onSurfaceVariant)
            }
            Spacer(modifier = Modifier.height(8.dp))
            Text(if (call.state == "dialing") tr("Aranıyor…") else "${callStateLabel(call)} · ${elapsed(call.since, now)}",
                 style = MaterialTheme.typography.titleMedium, color = if (call.held) cs.tertiary else cs.primary)
            Spacer(modifier = Modifier.height(4.dp))
            Text(if (app.callAudioOn()) app.callAudioStatus() else trf("Ses: {} (telefon)", routeLabel(call.route)),
                 style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, textAlign = TextAlign.Center)
            Spacer(modifier = Modifier.height(28.dp))
            if (keypad) {
                Text(if (typed.isEmpty()) " " else typed, style = MaterialTheme.typography.headlineSmall)
                Spacer(modifier = Modifier.height(8.dp))
                for (row in keypadRows()) {
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        for (key in row) {
                            Box(modifier = Modifier.clickable(onClick = {
                                    typed = typed + key
                                    app.callControl("call.dtmf", "{\"digit\":\"" + key + "\"}")
                                }).size(64.dp).background(cs.surfaceVariant, CircleShape),
                                contentAlignment = Alignment.Center) {
                                Text(key, style = MaterialTheme.typography.headlineSmall)
                            }
                        }
                    }
                    Spacer(modifier = Modifier.height(12.dp))
                }
                TextButton(onClick = { keypad = false }) { Text(tr("Tuş takımını gizle")) }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    RoundAction(if (call.muted) AppIcons.MicOff else AppIcons.Mic, if (call.muted) tr("Sessizde") else tr("Sessiz"),
                                call.muted, true, { app.callControl("call.mute", if (call.muted) "{\"on\":false}" else "{\"on\":true}") })
                    RoundAction(AppIcons.Dialpad, tr("Tuş takımı"), false, call.fullControl, { keypad = true })
                    RoundAction(if (call.held) Icons.Filled.PlayArrow else Icons.Filled.Pause, if (call.held) tr("Sürdür") else tr("Beklet"),
                                call.held, call.fullControl && call.canHold,
                                { app.callControl("call.hold", if (call.held) "{\"on\":false}" else "{\"on\":true}") })
                }
                Spacer(modifier = Modifier.height(16.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box {
                        RoundAction(if (call.route == "bluetooth") AppIcons.Bluetooth else AppIcons.VolumeUp,
                                    routeLabel(call.route), call.route == "speaker", call.routes.size > 1, { routes = true })
                        DropdownMenu(expanded = routes, onDismissRequest = { routes = false }) {
                            for (r in call.routes) {
                                DropdownMenuItem(text = { Text(routeLabel(r)) },
                                                 onClick = {
                                                     routes = false
                                                     app.callControl("call.route", routeBody(r))
                                                 },
                                                 leadingIcon = { if (call.route == r) Icon(Icons.Filled.Check, contentDescription = null) })
                            }
                        }
                    }
                    RoundAction(AppIcons.Computer, if (app.callAudioOn()) tr("Bilgisayarda") else tr("Bilgisayardan konuş"),
                                app.callAudioOn(), app.canBridgeCallAudio(),
                                { if (app.callAudioOn()) app.stopCallAudio() else app.startCallAudio() })
                    RoundAction(AppIcons.Smartphone, tr("Telefona al"), false, app.callAudioOn(), { app.stopCallAudio() })
                }
                Spacer(modifier = Modifier.height(16.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = { app.callControl("call.volume", "{\"delta\":-1}") }) {
                        Icon(AppIcons.VolumeDown, contentDescription = tr("Sesi azalt"))
                    }
                    LinearProgressIndicator(progress = { if (call.volumeMax > 0) call.volume.toFloat() / call.volumeMax else 0f },
                                            modifier = Modifier.width(160.dp))
                    IconButton(onClick = { app.callControl("call.volume", "{\"delta\":1}") }) {
                        Icon(AppIcons.VolumeUp, contentDescription = tr("Sesi artır"))
                    }
                }
            }
        }
        Box(modifier = Modifier.clickable(onClick = { app.callControl("call.hangup", "{}") }).size(72.dp).background(cs.error, CircleShape),
            contentAlignment = Alignment.Center) {
            Icon(AppIcons.CallEnd, contentDescription = tr("Aramayı kapat"), tint = cs.onError, modifier = Modifier.size(32.dp))
        }
        Spacer(modifier = Modifier.height(12.dp))
        if (!call.fullControl) {
            Text(tr("Bekletme ve tuş takımı için telefonda AirLink'in arama yardımcısı olarak bağlanması gerekiyor."),
                 style = MaterialTheme.typography.labelSmall, color = cs.onSurfaceVariant, textAlign = TextAlign.Center,
                 modifier = Modifier.padding(horizontal = 24.dp))
        }
        Spacer(modifier = Modifier.height(20.dp))
    }
}

// A paired phone rings: who, and answer (on the phone) or decline from here.
// Previews (KOMPOZ_PREVIEW=CallScreenPreview): the call screen with a made-up call.
@Preview(widthDp = 880, heightDp = 820)
@Kompozable
fun CallScreenPreview() {
    MaterialTheme {
        CallScreen(appState(), ActiveCall(peerId = "", number = "+90 555 123 45 67", name = "Ayşe Yılmaz", state = "active",
                   canHold = true, route = "earpiece", routes = listOf("earpiece", "speaker", "bluetooth"), volume = 4, volumeMax = 7,
                   since = nowMs() - 83000, fullControl = true), onMinimize = {})
    }
}

@Preview(widthDp = 420, heightDp = 620)
@Kompozable
fun IncomingCallPreview() {
    MaterialTheme {
        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            IncomingCallCard(appState(), IncomingCall(peerId = "", number = "+90 555 123 45 67", name = "Ayşe Yılmaz"))
        }
    }
}

// A paired phone rings: answer (here, or on the phone), decline, or decline with a message.
@Kompozable
fun IncomingCallDialog(app: App) {
    val c = app.incomingCall()
    if (c != null) {
        Dialog(onDismissRequest = { app.dismissCall() }) { IncomingCallCard(app, c!!) }
    }
}

@Kompozable
fun IncomingCallCard(app: App, call: IncomingCall) {
    val cs = MaterialTheme.colorScheme
    var replies by remember { mutableStateOf(false) }
            Card(modifier = Modifier.widthIn(0.dp, 380.dp)) {
                Column(modifier = Modifier.fillMaxWidth().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    Text(trf("Gelen arama · {}", app.peerName(call.peerId)), style = MaterialTheme.typography.labelLarge, color = cs.primary)
                    Spacer(modifier = Modifier.height(20.dp))
                    CallAvatar(call.title(), 96)
                    Spacer(modifier = Modifier.height(16.dp))
                    Text(call.title(), style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center, maxLines = 2)
                    if (call.name.isNotEmpty() && call.number.isNotEmpty()) {
                        Text(call.number, style = MaterialTheme.typography.bodyLarge, color = cs.onSurfaceVariant)
                    }
                    Spacer(modifier = Modifier.height(28.dp))
                    if (replies) {
                        for (text in quickReplies()) {
                            OutlinedButton(onClick = {
                                    app.rejectCall()
                                    if (call.number.isNotEmpty()) app.sendSms(call.number, text)
                                }, modifier = Modifier.fillMaxWidth().padding(vertical = 3.dp)) { Text(text) }
                        }
                        TextButton(onClick = { replies = false }) { Text(tr("Geri")) }
                    } else {
                        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Box(modifier = Modifier.clickable(onClick = { app.rejectCall() }).size(68.dp).background(cs.error, CircleShape),
                                    contentAlignment = Alignment.Center) {
                                    Icon(AppIcons.CallEnd, contentDescription = tr("Reddet"), tint = cs.onError, modifier = Modifier.size(30.dp))
                                }
                                Spacer(modifier = Modifier.height(6.dp))
                                Text(tr("Reddet"), style = MaterialTheme.typography.labelMedium)
                            }
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Box(modifier = Modifier.clickable(onClick = { app.answerCall() }).size(68.dp).background(Color(0xFF2E7D32), CircleShape),
                                    contentAlignment = Alignment.Center) {
                                    Icon(AppIcons.Phone, contentDescription = tr("Yanıtla"), tint = Color.White, modifier = Modifier.size(30.dp))
                                }
                                Spacer(modifier = Modifier.height(6.dp))
                                Text(tr("Yanıtla"), style = MaterialTheme.typography.labelMedium)
                            }
                        }
                        Spacer(modifier = Modifier.height(20.dp))
                        if (app.canBridgeCallAudio()) {
                            FilledTonalButton(onClick = { app.answerCallOnComputer() }, modifier = Modifier.fillMaxWidth()) {
                                Icon(AppIcons.Computer, contentDescription = null, modifier = Modifier.size(18.dp))
                                Spacer(modifier = Modifier.width(8.dp))
                                Text(tr("Yanıtla, bilgisayardan konuş"))
                            }
                        }
                        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                            if (call.number.isNotEmpty()) {
                                TextButton(onClick = { replies = true }) {
                                    Icon(AppIcons.Sms, contentDescription = null, modifier = Modifier.size(18.dp))
                                    Spacer(modifier = Modifier.width(6.dp))
                                    Text(tr("Mesajla reddet"))
                                }
                            }
                            TextButton(onClick = { app.dismissCall() }) { Text(tr("Gizle")) }
                        }
                    }
                }
            }
}

@Kompozable
fun PhoneScreen(app: App) {
    var tab by remember { mutableStateOf(0) }
    Column(modifier = Modifier.fillMaxSize()) {
        PrimaryTabRow(selectedTabIndex = tab) {
            Tab(selected = tab == 0, onClick = { tab = 0 }, text = { Text(tr("Aramalar")) })
            Tab(selected = tab == 1, onClick = { tab = 1 }, text = { Text(tr("Rehber")) })
        }
        if (tab == 0) CallsScreen(app) else ContactsScreen(app)
    }
}

@Kompozable
fun CallsScreen(app: App) {
    val phone = app.smsPhone()
    val log = app.callLog()
    val loading = app.callLogLoading()
    val list = rememberLazyListState()
    val nearEnd by remember {
        derivedStateOf {
            val info = list.layoutInfo
            info.visibleItemsInfo.isNotEmpty() && info.visibleItemsInfo.last().index >= info.totalItemsCount - 4
        }
    }
    var number by remember { mutableStateOf("") }
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestCallLog()
    }
    LaunchedEffect(nearEnd, log.size, loading) {
        if (nearEnd && !loading && app.callLogHasMore()) app.loadMoreCallLog()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Phone, tr("Telefon bağlı değil"), tr("Aramalar için eşleşmiş bir Android telefon çevrimiçi olmalı."))
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = number, onValueChange = { number = it }, singleLine = true, label = tr("Numara"),
                                  modifier = Modifier.weight(1f))
                Spacer(modifier = Modifier.width(8.dp))
                Button(onClick = { app.dial(number) }, enabled = number.isNotBlank()) { Text(tr("Telefondan ara")) }
            }
            if (app.callsError().isNotEmpty()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(app.callsError())
                    TextButton(onClick = { app.requestCallLog() }) { Text(tr("Tekrar dene")) }
                }
            }
            LazyColumn(modifier = Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(12.dp, 0.dp, 12.dp, 12.dp),
                       verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (log.isEmpty() && app.callsError().isEmpty()) {
                    item { Hint(if (loading) tr("Arama kaydı telefondan alınıyor…") else tr("Arama kaydı yok."), loading = loading) }
                }
                items(log) { c -> CallRow(c, onCall = { app.dial(c.number) }) }
                if (loading && log.isNotEmpty()) {
                    item { Hint(tr("Yükleniyor…"), loading = true) }
                }
            }
        }
    }
}

@Kompozable
fun ContactsScreen(app: App) {
    val phone = app.smsPhone()
    var query by remember { mutableStateOf("") }
    val list = rememberLazyListState()
    LaunchedEffect(phone + "|" + query) {
        if (phone.isNotEmpty()) {
            list.scrollToItem(0)
            delay(250)
            app.requestContacts(query)
        }
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(Icons.Filled.Person, tr("Telefon bağlı değil"), tr("Rehber için eşleşmiş bir Android telefon çevrimiçi olmalı."))
        }
    } else {
        val contacts = app.phoneContacts()
        val loading = app.phoneContactsLoading()
        val error = app.phoneContactsError()
        val nearEnd by remember {
            derivedStateOf {
                val info = list.layoutInfo
                info.visibleItemsInfo.isNotEmpty() && info.visibleItemsInfo.last().index >= info.totalItemsCount - 4
            }
        }
        LaunchedEffect(nearEnd, contacts.size, loading) {
            if (nearEnd && !loading && app.phoneContactsHasMore()) app.loadMoreContacts()
        }
        Column(modifier = Modifier.fillMaxSize()) {
            OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = tr("Rehberde ara"),
                              modifier = Modifier.fillMaxWidth().padding(12.dp))
            if (error.isNotEmpty()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text(error)
                    TextButton(onClick = { app.requestContacts(query) }) { Text(tr("Tekrar dene")) }
                }
            }
            LazyColumn(modifier = Modifier.fillMaxSize(), state = list, contentPadding = PaddingValues(12.dp, 0.dp, 12.dp, 12.dp),
                       verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (contacts.isEmpty() && error.isEmpty()) {
                    item { Hint(if (loading) tr("Rehber telefondan alınıyor…") else tr("Kişi bulunamadı."), loading = loading) }
                }
                items(contacts) { c -> PhoneContactRow(c, onCall = { app.dial(c.number) }) }
                if (loading && contacts.isNotEmpty()) {
                    item {
                        Hint(tr("Yükleniyor…"), loading = true)
                    }
                }
            }
        }
    }
}

@Kompozable
fun PhoneContactRow(c: ContactMatch, onCall: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(if (c.name.isEmpty()) c.number else c.name, style = MaterialTheme.typography.titleSmall)
                Text(if (c.label.isEmpty()) c.number else "${c.label} · ${c.number}",
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            TextButton(onClick = onCall) { Text(tr("Ara")) }
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
        "missed" -> tr("✕ Cevapsız")
        "rejected" -> "✕ Reddedildi"
        "blocked" -> "⊘ Engellendi"
        else -> tr("Arama")
    }
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(modifier = Modifier.weight(1f)) {
                Text(c.title(), style = MaterialTheme.typography.titleSmall, color = if (missed) cs.error else cs.onSurface)
                Text("$kind · ${shortDate(c.date)}${if (c.duration > 0) " · " + duration(c.duration) else ""}",
                     style = MaterialTheme.typography.bodySmall, color = if (missed) cs.error else cs.onSurfaceVariant)
            }
            TextButton(onClick = onCall) { Text(tr("Ara")) }
        }
    }
}

// ---- Messages (a paired phone's SMS, on a computer) -----------------------------------------
