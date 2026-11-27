// AirLink's UI. Files and clipboard: the history of what went between devices.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// Files sent and received: open, show in the file manager, send on, delete.
@Kompozable
fun FilesScreen(app: App) {
    val files = app.fileHistory()
    if (files.isEmpty()) {
        EmptyState(AppIcons.Folder, tr("Dosya geçmişi boş"), tr("Eşleşmiş cihazlarla gönderdiğiniz ve aldığınız dosyalar burada görünür."))
        return
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(files) { f -> FileRow(app, f, fileExists(f.path)) }  // (looked up here: a delete recomposes the list)
    }
}

@Preview(widthDp = 560, heightDp = 260)
@Kompozable
fun FileRowPreview() {
    MaterialTheme {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            FileRow(appState(), FileRecord(id = 1, peerName = "Xiaomi 14 Ultra", path = "/etc/hostname", size = 2457600, time = nowMs(), incoming = true), true)
            FileRow(appState(), FileRecord(id = 2, peerName = "cachyPC", path = "/tmp/yok/IMG_20261126_0942.jpg", size = 0, time = nowMs() - 86400000, incoming = false), false)
        }
    }
}

@Kompozable
fun FileRow(app: App, f: FileRecord, exists: Boolean) {
    val cs = MaterialTheme.colorScheme
    var shareMenu by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }
    val targets = app.pairedPeers().filter { it.online }
    // A deleted file keeps its card, tinted, until its "x" takes it off the list.
    Card(modifier = Modifier.fillMaxWidth(),
         colors = CardDefaults.cardColors(containerColor = if (exists) cs.surfaceContainerHigh else cs.errorContainer.copy(alpha = 0.6f))) {
        Row(modifier = Modifier.fillMaxWidth().padding(start = 12.dp, top = 8.dp, end = 4.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(40.dp).background(if (exists) cs.primaryContainer else cs.errorContainer, RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center) {
                Icon(if (!exists) Icons.Filled.Delete else if (f.incoming) AppIcons.Download else AppIcons.Upload, contentDescription = null,
                     tint = if (exists) cs.onPrimaryContainer else cs.onErrorContainer)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(f.name(), style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis,
                     color = if (exists) cs.onSurface else cs.onSurfaceVariant)
                Text(fileLine(f, exists), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2,
                     overflow = TextOverflow.Ellipsis)
            }
            IconButton(onClick = { openFile(f.path) }, enabled = exists) { Icon(AppIcons.OpenInNew, contentDescription = tr("Aç")) }
            // (a file sent from another app on the phone is that app's: no folder of AirLink's, not deleted here)
            IconButton(onClick = { showInFolder(f.path) }, enabled = !f.isUri()) {
                Icon(AppIcons.Folder, contentDescription = tr("Dosya yöneticisinde göster"))
            }
            Box {
                IconButton(onClick = { shareMenu = true }, enabled = exists) { Icon(Icons.Filled.Share, contentDescription = tr("Paylaş")) }
                DropdownMenu(expanded = shareMenu, onDismissRequest = { shareMenu = false }) {
                    for (peer in targets) {
                        DropdownMenuItem(text = { Text(trf("{} cihazına gönder", peer.name)) },
                                         onClick = {
                                             shareMenu = false
                                             app.sendFiles(peer.id, listOf(f.path))
                                         },
                                         leadingIcon = { Icon(AppIcons.forPlatform(peer.platform), contentDescription = null) })
                    }
                    if (!isDesktop()) {
                        DropdownMenuItem(text = { Text(tr("Uygulamalarla paylaş")) },
                                         onClick = {
                                             shareMenu = false
                                             shareWithApps(f.path)
                                         },
                                         leadingIcon = { Icon(Icons.Filled.Share, contentDescription = null) })
                    }
                    if (targets.isEmpty() && isDesktop()) {
                        DropdownMenuItem(text = { Text(tr("Çevrimiçi cihaz yok")) }, onClick = { shareMenu = false }, enabled = false)
                    }
                }
            }
            IconButton(onClick = { confirmDelete = true }, enabled = exists && !f.isUri()) {
                Icon(Icons.Filled.Delete, contentDescription = tr("Dosyayı sil"))
            }
            IconButton(onClick = { app.forgetFileRecord(f.id) }) { Icon(Icons.Filled.Close, contentDescription = tr("Geçmişten kaldır")) }
        }
    }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text(trf("{} silinsin mi?", f.name())) },
            text = {
                Text(if (f.incoming) tr("Dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır.")
                     else tr("Gönderdiğiniz özgün dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır."))
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmDelete = false
                    app.deleteFile(f.id)
                }) { Text(tr("Sil")) }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text(tr("Vazgeç")) } },
        )
    }
}

// ---- Cloud drives (a computer): a tab per connected account ----------------------------------

// What went through the clipboard share: copy it again, open its link, or remove it.
@Kompozable
fun ClipboardScreen(app: App, clipboard: ClipboardManager) {
    val items = app.clipboardHistory()
    // Passwords go as the list leaves the screen (another tab, the window closed, the app gone).
    DisposableEffect(Unit) {
        onDispose { app.dropPasswords() }
    }
    if (items.isEmpty()) {
        EmptyState(AppIcons.ContentPaste, tr("Pano geçmişi boş"),
                   tr("Eşleşmiş cihazlarla paylaşılan metinler ve bağlantılar burada görünür."))
        return
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        items(items) { item -> ClipboardRow(app, item, clipboard) }
    }
}

@Kompozable
fun ClipboardRow(app: App, item: ClipboardItem, clipboard: ClipboardManager) {
    val cs = MaterialTheme.colorScheme
    val links = uriHandler()
    val link = if (item.sensitive) "" else firstLink(item.text)
    val peer = app.peerName(item.peerId)
    // A password: masked, on a card of its own color; copying it takes it off the list.
    Card(modifier = Modifier.fillMaxWidth(),
         colors = CardDefaults.cardColors(containerColor = if (item.sensitive) cs.tertiaryContainer else cs.surfaceContainerHigh)) {
        Column(modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, end = 8.dp, bottom = 4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (item.sensitive) {
                    Icon(Icons.Filled.Lock, contentDescription = null, modifier = Modifier.size(14.dp), tint = cs.onTertiaryContainer)
                    Spacer(modifier = Modifier.width(6.dp))
                }
                Text((if (item.sensitive) tr("Şifre · ") else "") +
                     (if (item.incoming) trf("{} cihazından · {}", peer, clockTime(item.time)) else trf("{} cihazına · {}", peer, clockTime(item.time))),
                     style = MaterialTheme.typography.labelMedium, color = if (item.sensitive) cs.onTertiaryContainer else cs.primary)
            }
            Spacer(modifier = Modifier.height(4.dp))
            Text(if (item.sensitive) "••••••••" else item.text, style = MaterialTheme.typography.bodyMedium, maxLines = 6,
                 overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(end = 8.dp))
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (link.isNotEmpty()) TextButton(onClick = { links.openUri(link) }) { Text(tr("Bağlantıyı aç")) }
                TextButton(onClick = {
                    clipboard.setText(item.text)
                    app.copiedClipboardItem(item.id)
                }) { Text(tr("Kopyala")) }
                TextButton(onClick = { app.removeClipboardItem(item.id) }) { Text(tr("Sil")) }
            }
        }
    }
}
