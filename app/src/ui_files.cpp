// AirLink's UI. Files and clipboard: the history of what went between devices.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// Files sent and received: open, show in the file manager, send on, delete; as a list or a grid,
// with a search by name.
@Kompozable
fun FilesScreen(app: App) {
    val cs = MaterialTheme.colorScheme
    val files = app.fileHistory()
    val grid = app.filesGrid()
    var query by remember { mutableStateOf("") }
    if (files.isEmpty()) {
        EmptyState(AppIcons.Folder, tr("Dosya geçmişi boş"), tr("Eşleşmiş cihazlarla gönderdiğiniz ve aldığınız dosyalar burada görünür."))
        return
    }
    val shown = if (query.isBlank()) files else files.filter { matchesQuery(it.name(), query) || matchesQuery(it.peerName, query) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().wrapContentWidth().widthIn(0.dp, 1100.dp).padding(start = 16.dp, end = 16.dp, top = 8.dp),
            verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = tr("Dosyalarda ara"),
                              modifier = Modifier.weight(1f),
                              leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
                              trailingIcon = {
                                  if (query.isNotEmpty()) {
                                      IconButton(onClick = { query = "" }) { Icon(Icons.Filled.Close, contentDescription = tr("Aramayı temizle")) }
                                  }
                              })
            Spacer(modifier = Modifier.width(8.dp))
            IconButton(onClick = { app.setFilesGrid(!grid) }) {
                Icon(if (grid) AppIcons.ViewList else AppIcons.GridView, contentDescription = if (grid) tr("Liste görünümü") else tr("Izgara görünümü"))
            }
        }
        if (shown.isEmpty()) {
            Text(tr("Eşleşen dosya yok"), color = cs.onSurfaceVariant, modifier = Modifier.padding(32.dp))
        } else if (grid) {
            LazyVerticalGrid(GridCells.Adaptive(150.dp), modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 1100.dp),
                             contentPadding = PaddingValues(16.dp),
                             verticalArrangement = Arrangement.spacedBy(10.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                items(shown) { f -> FileTile(app, f, fileExists(f.path)) }
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 900.dp),
                contentPadding = PaddingValues(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(shown) { f -> FileRow(app, f, fileExists(f.path)) }  // (looked up here: a delete recomposes the list)
            }
        }
    }
}

// A file's picture: an image's own (loaded small, away from the UI thread), else its kind's icon
// in its color; a deleted file's is the bin. A badge on its corner says whether it came or went.
@Kompozable
fun FilePicture(f: FileRecord, exists: Boolean, big: Boolean) {
    val cs = MaterialTheme.colorScheme
    val colored = exists && hasFileKindColor(f.name(), "")
    val tone = fileKindColor(f.name(), "")
    val iconSize = if (big) 48.dp else 24.dp
    val shape = RoundedCornerShape(if (big) 12.dp else 8.dp)
    val box = if (big) Modifier.fillMaxWidth().aspectRatio(1f) else Modifier.size(40.dp)
    Box(modifier = box) {
        Box(modifier = Modifier.fillMaxSize().clip(shape)
                         .background(if (!exists) cs.errorContainer else if (colored) tone.copy(alpha = 0.16f) else cs.primaryContainer),
            contentAlignment = Alignment.Center) {
            if (!exists) {
                Icon(Icons.Filled.Delete, contentDescription = null, tint = cs.onErrorContainer, modifier = Modifier.size(iconSize))
            } else if (!f.isUri() && showsOwnPicture(f.name())) {
                AsyncImage(model = f.path, contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop,
                           placeholder = { Icon(AppIcons.Image, contentDescription = null, tint = cs.onPrimaryContainer, modifier = Modifier.size(iconSize)) },
                           error = { Icon(AppIcons.Image, contentDescription = null, tint = cs.onPrimaryContainer, modifier = Modifier.size(iconSize)) })
            } else {
                Icon(fileKindIcon(f.name(), ""), contentDescription = null, tint = if (colored) tone else cs.onPrimaryContainer, modifier = Modifier.size(iconSize))
            }
        }
        if (exists) {
            val badge = if (big) 24.dp else 18.dp
            Box(modifier = Modifier.size(badge).align(Alignment.BottomEnd).offset(x = if (big) (-6).dp else 4.dp, y = if (big) (-6).dp else 4.dp)
                               .background(cs.surfaceContainerHigh, CircleShape).padding(2.dp)
                               .background(if (f.incoming) cs.primary else cs.tertiary, CircleShape),
                contentAlignment = Alignment.Center) {
                Icon(if (f.incoming) AppIcons.Download else AppIcons.Upload, contentDescription = if (f.incoming) tr("Gelen") else tr("Giden"),
                     tint = if (f.incoming) cs.onPrimary else cs.onTertiary, modifier = Modifier.size(if (big) 14.dp else 10.dp))
            }
        }
    }
}

// A grid cell: the picture, the name and where it went, and every action in its menu.
@Kompozable
fun FileTile(app: App, f: FileRecord, exists: Boolean) {
    val cs = MaterialTheme.colorScheme
    var menu by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }
    val targets = app.pairedPeers().filter { it.online }
    Card(modifier = Modifier.fillMaxWidth().clickable { if (exists) openFile(f.path) },
         colors = CardDefaults.cardColors(containerColor = if (exists) cs.surfaceContainerHigh else cs.errorContainer.copy(alpha = 0.6f))) {
        Column(modifier = Modifier.fillMaxWidth().padding(8.dp)) {
            FilePicture(f, exists, true)
            Row(modifier = Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f).padding(start = 4.dp)) {
                    Text(f.name(), style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis,
                         color = if (exists) cs.onSurface else cs.onSurfaceVariant)
                    Text(if (exists) f.peerName else tr("dosya artık yok"), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant,
                         maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                Box {
                    IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = tr("Diğer")) }
                    DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                        DropdownMenuItem(text = { Text(tr("Aç")) }, enabled = exists,
                                         onClick = {
                                             menu = false
                                             openFile(f.path)
                                         },
                                         leadingIcon = { Icon(AppIcons.OpenInNew, contentDescription = null) })
                        if (!f.isUri()) {
                            DropdownMenuItem(text = { Text(tr("Dosya yöneticisinde göster")) },
                                             onClick = {
                                                 menu = false
                                                 showInFolder(f.path)
                                             },
                                             leadingIcon = { Icon(AppIcons.Folder, contentDescription = null) })
                        }
                        if (exists) {
                            for (peer in targets) {
                                DropdownMenuItem(text = { Text(trf("{} cihazına gönder", peer.name)) },
                                                 onClick = {
                                                     menu = false
                                                     app.sendFiles(peer.id, listOf(f.path))
                                                 },
                                                 leadingIcon = { Icon(AppIcons.forPlatform(peer.platform), contentDescription = null) })
                            }
                            if (!isDesktop()) {
                                DropdownMenuItem(text = { Text(tr("Uygulamalarla paylaş")) },
                                                 onClick = {
                                                     menu = false
                                                     shareWithApps(f.path)
                                                 },
                                                 leadingIcon = { Icon(Icons.Filled.Share, contentDescription = null) })
                            }
                        }
                        if (exists && !f.isUri()) {
                            DropdownMenuItem(text = { Text(tr("Dosyayı sil")) },
                                             onClick = {
                                                 menu = false
                                                 confirmDelete = true
                                             },
                                             leadingIcon = { Icon(Icons.Filled.Delete, contentDescription = null) })
                        }
                        DropdownMenuItem(text = { Text(tr("Geçmişten kaldır")) },
                                         onClick = {
                                             menu = false
                                             app.forgetFileRecord(f.id)
                                         },
                                         leadingIcon = { Icon(Icons.Filled.Close, contentDescription = null) })
                    }
                }
            }
        }
    }
    if (confirmDelete) FileDeleteDialog(app, f, onDone = { confirmDelete = false })
}

@Kompozable
fun FileDeleteDialog(app: App, f: FileRecord, onDone: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDone,
        title = { Text(trf("{} silinsin mi?", f.name())) },
        text = {
            Text(if (f.incoming) tr("Dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır.")
                 else tr("Gönderdiğiniz özgün dosya bu cihazdan kalıcı olarak silinir. Kaydı geçmişte kalır."))
        },
        confirmButton = {
            TextButton(onClick = {
                onDone()
                app.deleteFile(f.id)
            }) { Text(tr("Sil")) }
        },
        dismissButton = { TextButton(onClick = onDone) { Text(tr("Vazgeç")) } },
    )
}

@Preview(widthDp = 560, heightDp = 400)
@Kompozable
fun FileRowPreview() {
    MaterialTheme {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            FileRow(appState(), FileRecord(id = 1, peerName = "Xiaomi 14 Ultra", path = "/etc/hostname", size = 2457600, time = nowMs(), incoming = true), true)
            FileRow(appState(), FileRecord(id = 3, peerName = "Xiaomi 14 Ultra", path = "/tmp/Rapor.pdf", size = 812000, time = nowMs(), incoming = true), true)
            FileRow(appState(), FileRecord(id = 4, peerName = "cachyPC", path = "/tmp/Yedek.zip", size = 52000000, time = nowMs(), incoming = false), true)
            FileRow(appState(), FileRecord(id = 2, peerName = "cachyPC", path = "/tmp/yok/IMG_20261126_0942.jpg", size = 0, time = nowMs() - 86400000, incoming = false), false)
        }
    }
}

@Preview(widthDp = 720, heightDp = 300)
@Kompozable
fun FileTilePreview() {
    val image = previewImagePath()  // (AIRLINK_PREVIEW_IMAGE: any picture to show)
    MaterialTheme {
        Row(modifier = Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Box(modifier = Modifier.width(160.dp)) { FileTile(appState(), FileRecord(id = 1, peerName = "Xiaomi 14 Ultra", path = image, size = 2457600, time = nowMs(), incoming = true), true) }
            Box(modifier = Modifier.width(160.dp)) { FileTile(appState(), FileRecord(id = 2, peerName = "cachyPC", path = "/tmp/Rapor.pdf", size = 812000, time = nowMs(), incoming = false), true) }
            Box(modifier = Modifier.width(160.dp)) { FileTile(appState(), FileRecord(id = 3, peerName = "Xiaomi 14 Ultra", path = "/tmp/Yedek.zip", size = 52000000, time = nowMs(), incoming = true), true) }
            Box(modifier = Modifier.width(160.dp)) { FileTile(appState(), FileRecord(id = 4, peerName = "cachyPC", path = "/tmp/yok/IMG.jpg", size = 0, time = nowMs(), incoming = false), false) }
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
            FilePicture(f, exists, false)
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
    if (confirmDelete) FileDeleteDialog(app, f, onDone = { confirmDelete = false })
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
