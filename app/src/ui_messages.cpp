// AirLink's UI. Messages: the phone's SMS conversations.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

@Kompozable
fun MessagesScreen(app: App) {
    val phone = app.smsPhone()
    val open = app.smsOpenThread()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty()) app.requestSmsThreads()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Sms, tr("Telefon bağlı değil"), tr("Mesajlar için eşleşmiş bir Android telefon çevrimiçi olmalı."))
        }
    } else if (app.smsError().isNotEmpty()) {
        Column(modifier = Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(app.smsError())
            TextButton(onClick = { app.requestSmsThreads() }) { Text(tr("Tekrar dene")) }
        }
    } else if (currentWindowAdaptiveInfo().windowSizeClass.windowWidthSizeClass == WindowWidthSizeClass.Expanded) {
        // A wide window: the threads and the open one side by side.
        Row(modifier = Modifier.fillMaxSize()) {
            Box(modifier = Modifier.width(360.dp).fillMaxHeight()) { ThreadList(app) }
            Box(modifier = Modifier.width(1.dp).fillMaxHeight().background(MaterialTheme.colorScheme.outlineVariant))
            Box(modifier = Modifier.weight(1f).fillMaxHeight()) {
                if (open != null) {
                    Conversation(app, open!!, showBack = false)
                } else {
                    EmptyState(AppIcons.Sms, tr("Bir konuşma seçin"), tr("Soldaki listeden bir konuşma açın ya da yeni mesaj yazın."))
                }
            }
        }
    } else if (open != null) {
        Conversation(app, open!!, showBack = true)
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
                item { Hint(if (app.smsLoading()) tr("Mesajlar telefondan alınıyor…") else tr("Mesaj yok."), loading = app.smsLoading()) }
            }
            items(threads) { t -> ThreadRow(t, onClick = { app.openSmsThread(t) }) }
        }
        FloatingActionButton(onClick = { composing = true }, modifier = Modifier.align(Alignment.BottomEnd).padding(16.dp)) {
            Icon(Icons.Filled.Add, contentDescription = tr("Yeni mesaj"))
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
fun Conversation(app: App, thread: SmsThread, showBack: Boolean) {
    val messages = app.smsMessages()
    val list = rememberLazyListState()
    var text by remember { mutableStateOf("") }
    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) list.scrollToItem(messages.size - 1)
    }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
            if (showBack) {
                IconButton(onClick = { app.closeSmsThread() }) { Icon(Icons.Filled.ArrowBack, contentDescription = tr("Geri")) }
            } else {
                Spacer(modifier = Modifier.width(12.dp))
            }
            Column(modifier = Modifier.padding(vertical = 6.dp)) {
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
            OutlinedTextField(value = text, onValueChange = { text = it }, modifier = Modifier.weight(1f), label = tr("Mesaj"))
            IconButton(onClick = { app.sendSms(thread.address, text); text = "" }, enabled = text.isNotBlank()) {
                Icon(Icons.Filled.Send, contentDescription = tr("Gönder"))
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
            Text(if (m.failed) trf("{} · gönderilemedi", clockTime(m.date)) else if (m.id < 0) tr("gönderiliyor…") else clockTime(m.date),
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
        title = { Text(tr("Yeni mesaj")) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = to, onValueChange = { to = it; chosen = ""; number = it }, singleLine = true,
                                  label = tr("Kişi ya da numara"), modifier = Modifier.fillMaxWidth())
                if (chosen.isNotEmpty()) {
                    Text("$chosen · $number", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.primary)
                } else {
                    for (c in results.take(5)) {
                        ContactRow(c, onClick = { chosen = c.name; number = c.number; to = c.name })
                    }
                }
                OutlinedTextField(value = body, onValueChange = { body = it }, label = tr("Mesaj"), modifier = Modifier.fillMaxWidth())
            }
        },
        confirmButton = {
            TextButton(onClick = { app.sendSms(number, body); app.searchContacts(""); app.requestSmsThreads(); onClose() },
                       enabled = number.isNotBlank() && body.isNotBlank()) { Text(tr("Gönder")) }
        },
        dismissButton = { TextButton(onClick = { app.searchContacts(""); onClose() }) { Text(tr("Vazgeç")) } },
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
