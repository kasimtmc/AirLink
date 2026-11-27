// AirLink's UI. Cloud drives: the Settings card and an account's tab.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

@Kompozable
fun CloudAccountsCard(drives: Drives) {
    val cs = MaterialTheme.colorScheme
    val accounts = drives.accounts()
    val connecting = drives.connecting()
    var removing by remember { mutableStateOf("") }
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("OneDrive, Dropbox ve Google Drive hesaplarınızı bağlayın; her hesap kendi sekmesinde açılır. Giriş tarayıcıda yapılır, oturum sistemin anahtar zincirinde saklanır."),
                 style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant)
            for (a in accounts) {
                Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Icon(AppIcons.Cloud, contentDescription = null, tint = cs.primary)
                    Spacer(modifier = Modifier.width(12.dp))
                    Column(modifier = Modifier.weight(1f)) {
                        Text(providerTitle(a.provider), style = MaterialTheme.typography.titleSmall)
                        Text(if (a.email.isEmpty()) a.name else "${a.name} · ${a.email}", style = MaterialTheme.typography.bodySmall,
                             color = cs.onSurfaceVariant, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    TextButton(onClick = { removing = a.id }) { Text(tr("Kaldır")) }
                }
            }
            if (connecting.isNotEmpty()) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                    Spacer(modifier = Modifier.width(12.dp))
                    Text(trf("{}: tarayıcıda giriş bekleniyor…", providerTitle(connecting)), modifier = Modifier.weight(1f))
                    TextButton(onClick = { drives.cancelConnect() }) { Text(tr("Vazgeç")) }
                }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (p in drives.providers()) {
                        OutlinedButton(onClick = { drives.connect(p) }) {
                            Icon(Icons.Filled.Add, contentDescription = null, modifier = Modifier.size(18.dp))
                            Spacer(modifier = Modifier.width(6.dp))
                            Text(providerTitle(p))
                        }
                    }
                }
            }
        }
    }
    if (removing.isNotEmpty()) {
        AlertDialog(
            onDismissRequest = { removing = "" },
            title = { Text(tr("Hesap kaldırılsın mı?")) },
            text = { Text(tr("AirLink bu hesabın oturumunu bu bilgisayardan siler. Sürücüdeki dosyalarınız olduğu gibi kalır.")) },
            confirmButton = {
                TextButton(onClick = {
                    drives.disconnect(removing)
                    removing = ""
                }) { Text(tr("Kaldır")) }
            },
            dismissButton = { TextButton(onClick = { removing = "" }) { Text(tr("Vazgeç")) } },
        )
    }
}

@Kompozable
fun CloudScreen(app: App, drives: Drives, account: CloudAccount) {
    LaunchedEffect(account.id) { drives.openRoot(account.id) }
    var page by remember(account.id) { mutableStateOf(0) }  // 0: the files, 1: the history
    val history = drives.history(account.id)
    Column(modifier = Modifier.fillMaxSize()) {
        PrimaryTabRow(selectedTabIndex = page) {
            Tab(selected = page == 0, onClick = { page = 0 }, text = { Text(tr("Dosyalar")) },
                icon = { Icon(AppIcons.Folder, contentDescription = null) })
            Tab(selected = page == 1, onClick = { page = 1 }, text = { Text(tr("Geçmiş")) },
                icon = { Icon(AppIcons.History, contentDescription = null) })
        }
        if (page == 0) CloudFiles(app, drives, account)
        else CloudHistory(app, drives, account, history)
    }
}

// The open folder (or a search) as a list or a grid, with the transfers running in it on top.
@Kompozable
fun CloudFiles(app: App, drives: Drives, account: CloudAccount) {
    val cs = MaterialTheme.colorScheme
    val view = drives.view(account.id)
    val grid = drives.layout() == CloudLayout.Grid
    val jobs = drives.jobs().filter { it.accountId == account.id }
    var query by remember(account.id) { mutableStateOf("") }
    var newFolder by remember { mutableStateOf(false) }
    var folderName by remember { mutableStateOf("") }
    var dragging by remember { mutableStateOf(false) }
    val drop = Modifier.dragAndDropTarget(
        shouldStartDragAndDrop = { e -> carriesFiles(e) },
        target = DragAndDropTarget(
            onDrop = { e ->
                dragging = false
                drives.upload(account.id, droppedFiles(e))
                true
            },
            onEntered = { dragging = true },
            onExited = { dragging = false },
            onEnded = { dragging = false },
        ),
    )
    Column(modifier = drop.fillMaxSize().background(if (dragging) cs.secondaryContainer else cs.surface)) {
        Column(modifier = Modifier.fillMaxWidth().wrapContentWidth().widthIn(0.dp, 1100.dp).padding(start = 16.dp, end = 16.dp, top = 8.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = tr("Sürücüde ara"),
                                  modifier = Modifier.weight(1f), onSubmit = { drives.search(account.id, query.trim()) },
                                  leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
                                  trailingIcon = {
                                      if (query.isNotEmpty() || view.search.isNotEmpty()) {
                                          IconButton(onClick = {
                                              query = ""
                                              drives.search(account.id, "")
                                          }) { Icon(Icons.Filled.Close, contentDescription = tr("Aramayı temizle")) }
                                      }
                                  })
                Spacer(modifier = Modifier.width(8.dp))
                FilledTonalButton(onClick = { drives.pickAndUpload(account.id) }) {
                    Icon(AppIcons.Upload, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(modifier = Modifier.width(6.dp))
                    Text(tr("Yükle"))
                }
                IconButton(onClick = {
                    folderName = ""
                    newFolder = true
                }) { Icon(AppIcons.CreateNewFolder, contentDescription = tr("Yeni klasör")) }
                IconButton(onClick = { drives.setLayout(if (grid) CloudLayout.List else CloudLayout.Grid) }) {
                    Icon(if (grid) AppIcons.ViewList else AppIcons.GridView, contentDescription = if (grid) tr("Liste görünümü") else tr("Izgara görünümü"))
                }
                IconButton(onClick = { drives.refresh(account.id) }) { Icon(Icons.Filled.Refresh, contentDescription = tr("Yenile")) }
            }
            // Where we are: the folders from the root, or the search.
            Row(modifier = Modifier.fillMaxWidth().padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                if (view.search.isNotEmpty()) {
                    Text(trf("\"{}\" için sonuçlar", view.search), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f).padding(vertical = 12.dp))
                } else {
                    Row(modifier = Modifier.weight(1f).horizontalScroll(rememberScrollState()), verticalAlignment = Alignment.CenterVertically) {
                        view.path.forEachIndexed { i, f ->
                            if (i > 0) Icon(Icons.Filled.KeyboardArrowRight, contentDescription = null, tint = cs.onSurfaceVariant)
                            TextButton(onClick = { drives.goUp(account.id, i) }, enabled = i < view.path.size - 1) {
                                Text(if (i == 0) providerTitle(account.provider) else f.name, maxLines = 1)
                            }
                        }
                    }
                }
                val quota = quotaText(view)
                if (quota.isNotEmpty()) {
                    Column(horizontalAlignment = Alignment.End, modifier = Modifier.widthIn(120.dp, 220.dp)) {
                        Text(quota, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 1)
                        if (view.total > 0) LinearProgressIndicator(progress = { quotaFraction(view) }, modifier = Modifier.fillMaxWidth().padding(top = 4.dp))
                    }
                }
            }
            if (view.loading) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            // Transfers: a finished one stays a few seconds, then it is in the history.
            if (jobs.isNotEmpty()) {
                Column(modifier = Modifier.fillMaxWidth().padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (j in jobs.take(4)) { CloudJobRow(drives, j) }
                }
            }
            if (view.error.isNotEmpty()) {
                Card(colors = CardDefaults.cardColors(containerColor = cs.errorContainer), modifier = Modifier.fillMaxWidth().padding(top = 8.dp)) {
                    Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(view.error, color = cs.onErrorContainer, modifier = Modifier.weight(1f))
                        TextButton(onClick = { drives.refresh(account.id) }) { Text(tr("Yeniden dene")) }
                    }
                }
            }
            if (!view.loading && view.error.isEmpty() && view.entries.isEmpty()) {
                Text(if (view.search.isNotEmpty()) tr("Eşleşen dosya yok") else tr("Bu klasör boş. Dosyaları buraya sürükleyin ya da Yükle'ye basın."),
                     color = cs.onSurfaceVariant, modifier = Modifier.padding(16.dp))
            }
        }
        if (grid) {
            LazyVerticalGrid(GridCells.Adaptive(150.dp), modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 1100.dp),
                             contentPadding = PaddingValues(16.dp),
                             verticalArrangement = Arrangement.spacedBy(10.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                items(view.entries) { e -> CloudEntryTile(app, drives, account, e) }
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 900.dp),
                contentPadding = PaddingValues(16.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                items(view.entries) { e -> CloudEntryRow(app, drives, account, e) }
            }
        }
    }
    if (newFolder) {
        AlertDialog(
            onDismissRequest = { newFolder = false },
            title = { Text(tr("Yeni klasör")) },
            text = {
                OutlinedTextField(value = folderName, onValueChange = { folderName = it }, singleLine = true, label = tr("Klasör adı"),
                                  modifier = Modifier.fillMaxWidth(),
                                  onSubmit = {
                                      newFolder = false
                                      drives.createFolder(account.id, folderName.trim())
                                  })
            },
            confirmButton = {
                TextButton(onClick = {
                    newFolder = false
                    drives.createFolder(account.id, folderName.trim())
                }, enabled = folderName.isNotBlank()) { Text(tr("Oluştur")) }
            },
            dismissButton = { TextButton(onClick = { newFolder = false }) { Text(tr("Vazgeç")) } },
        )
    }
}

// The account's finished downloads and uploads, newest first.
@Kompozable
fun CloudHistory(app: App, drives: Drives, account: CloudAccount, history: List<CloudRecord>) {
    val cs = MaterialTheme.colorScheme
    if (history.isEmpty()) {
        EmptyState(AppIcons.History, tr("Geçmiş boş"), tr("Bu hesaba yüklediğiniz ve buradan indirdiğiniz dosyalar burada görünür."))
        return
    }
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 900.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        item {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(trf("{} kayıt", history.size), style = MaterialTheme.typography.labelLarge, color = cs.onSurfaceVariant, modifier = Modifier.weight(1f))
                TextButton(onClick = { drives.clearHistory(account.id) }) { Text(tr("Geçmişi temizle")) }
            }
        }
        items(history) { r -> CloudRecordRow(app, drives, r, r.path.isNotEmpty() && fileExists(r.path)) }
    }
}

@Kompozable
fun CloudRecordRow(app: App, drives: Drives, r: CloudRecord, exists: Boolean) {
    val cs = MaterialTheme.colorScheme
    val failed = r.error.isNotEmpty()
    Card(modifier = Modifier.fillMaxWidth(), colors = CardDefaults.cardColors(containerColor = cs.surfaceContainerHigh)) {
        Row(modifier = Modifier.fillMaxWidth().padding(start = 12.dp, top = 6.dp, end = 4.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.size(40.dp).background(if (failed) cs.errorContainer else cs.primaryContainer, RoundedCornerShape(8.dp)),
                contentAlignment = Alignment.Center) {
                Icon(if (r.upload) AppIcons.Upload else AppIcons.Download, contentDescription = null,
                     tint = if (failed) cs.onErrorContainer else cs.onPrimaryContainer)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(r.name, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(cloudRecordLine(r, app.peerName(r.sendTo)), style = MaterialTheme.typography.bodySmall,
                     color = if (failed) cs.error else cs.onSurfaceVariant, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            if (!r.upload && r.sendTo.isEmpty() && !failed) {
                IconButton(onClick = { openFile(r.path) }, enabled = exists) { Icon(AppIcons.OpenInNew, contentDescription = tr("Aç")) }
                IconButton(onClick = { showInFolder(r.path) }, enabled = exists) {
                    Icon(AppIcons.Folder, contentDescription = tr("Dosya yöneticisinde göster"))
                }
            }
            IconButton(onClick = { drives.forgetRecord(r.id) }) { Icon(Icons.Filled.Close, contentDescription = tr("Geçmişten kaldır")) }
        }
    }
}

@Kompozable
fun CloudJobRow(drives: Drives, j: CloudJob) {
    val cs = MaterialTheme.colorScheme
    val tint = if (j.error.isNotEmpty()) cs.error else if (j.finished) Color(0xFF2E7D32) else cs.primary
    Card(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(start = 16.dp, top = 8.dp, end = 4.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(if (j.upload) AppIcons.Upload else AppIcons.Download, contentDescription = null, tint = tint)
            Spacer(modifier = Modifier.width(16.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(j.name, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (j.active()) LinearProgressIndicator(progress = { j.fraction() }, modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp))
                Text(cloudJobLine(j), style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 2)
            }
            if (j.active()) {
                IconButton(onClick = { drives.cancelJob(j.op) }) { Icon(Icons.Filled.Close, contentDescription = tr("İptal")) }
            } else if (!j.upload && j.finished && j.sendTo.isEmpty()) {
                IconButton(onClick = { drives.openJobFile(j) }) { Icon(AppIcons.OpenInNew, contentDescription = tr("Aç")) }
            }
        }
    }
}

// An entry's picture: its thumbnail once it is in (asked for as it shows), else its kind's icon.
@Kompozable
fun CloudEntryPicture(drives: Drives, account: CloudAccount, e: CloudEntry, size: Dp, big: Boolean) {
    val cs = MaterialTheme.colorScheme
    LaunchedEffect(account.id, e.id, e.modified) { drives.requestThumbnail(account.id, e) }
    val thumb = drives.thumbnail(account.id, e)
    val shape = RoundedCornerShape(if (big) 12.dp else 8.dp)
    val box = if (big) Modifier.fillMaxWidth().aspectRatio(1f) else Modifier.size(size)
    val plain = e.folder || !hasFileKindColor(e.name, e.kind)
    Box(modifier = box.clip(shape).background(if (e.folder) cs.secondaryContainer else if (plain) cs.primaryContainer else cs.surfaceContainerHighest),
        contentAlignment = Alignment.Center) {
        if (!thumb.empty()) {
            Image(thumb, contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize())
            if (isVideo(e.name)) {
                Icon(AppIcons.PlayCircle, contentDescription = null, tint = Color.White.copy(alpha = 0.9f),
                     modifier = Modifier.size(if (big) 40.dp else 20.dp))
            }
        } else if (e.folder) {
            Icon(AppIcons.Folder, contentDescription = null, tint = cs.onSecondaryContainer, modifier = Modifier.size(if (big) 48.dp else 24.dp))
        } else if (hasFileKindColor(e.name, e.kind)) {
            // (its kind's color, over a light wash of it)
            val tone = fileKindColor(e.name, e.kind)
            Box(modifier = Modifier.fillMaxSize().background(tone.copy(alpha = 0.16f)), contentAlignment = Alignment.Center) {
                Icon(fileKindIcon(e.name, e.kind), contentDescription = null, tint = tone, modifier = Modifier.size(if (big) 48.dp else 24.dp))
            }
        } else {
            Icon(fileKindIcon(e.name, e.kind), contentDescription = null, tint = cs.onPrimaryContainer, modifier = Modifier.size(if (big) 48.dp else 24.dp))
        }
    }
}

@Kompozable
fun CloudEntryRow(app: App, drives: Drives, account: CloudAccount, e: CloudEntry) {
    val cs = MaterialTheme.colorScheme
    var sendMenu by remember { mutableStateOf(false) }
    val targets = app.pairedPeers().filter { it.online }
    Card(modifier = Modifier.fillMaxWidth().clickable { if (e.folder) drives.open(account.id, e) },
         colors = CardDefaults.cardColors(containerColor = cs.surfaceContainerHigh)) {
        Row(modifier = Modifier.fillMaxWidth().padding(start = 12.dp, top = 6.dp, end = 4.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            CloudEntryPicture(drives, account, e, 40.dp, false)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(e.name, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                val line = cloudEntryLine(e)
                if (line.isNotEmpty()) Text(line, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 1)
            }
            if (!e.folder) {
                IconButton(onClick = { drives.download(account.id, e) }) { Icon(AppIcons.Download, contentDescription = tr("İndir")) }
                Box {
                    IconButton(onClick = { sendMenu = true }) { Icon(Icons.Filled.Send, contentDescription = tr("Cihaza gönder")) }
                    DropdownMenu(expanded = sendMenu, onDismissRequest = { sendMenu = false }) {
                        for (peer in targets) {
                            DropdownMenuItem(text = { Text(trf("{} cihazına gönder", peer.name)) },
                                             onClick = {
                                                 sendMenu = false
                                                 drives.sendToDevice(account.id, e, peer.id)
                                             },
                                             leadingIcon = { Icon(AppIcons.forPlatform(peer.platform), contentDescription = null) })
                        }
                        if (targets.isEmpty()) DropdownMenuItem(text = { Text(tr("Çevrimiçi cihaz yok")) }, onClick = { sendMenu = false }, enabled = false)
                    }
                }
            }
            CloudEntryMenu(app, drives, account, e, false)
        }
    }
}

// A grid cell: the picture on top, the name and the menu under it.
@Kompozable
fun CloudEntryTile(app: App, drives: Drives, account: CloudAccount, e: CloudEntry) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth().clickable { if (e.folder) drives.open(account.id, e) },
         colors = CardDefaults.cardColors(containerColor = cs.surfaceContainerHigh)) {
        Column(modifier = Modifier.fillMaxWidth().padding(8.dp)) {
            CloudEntryPicture(drives, account, e, 0.dp, true)
            Row(modifier = Modifier.fillMaxWidth().padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f).padding(start = 4.dp)) {
                    Text(e.name, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    val line = if (e.folder) tr("Klasör") else if (!e.kind.isEmpty()) tr("Google dokümanı") else humanSize(e.size)
                    Text(line, style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant, maxLines = 1)
                }
                CloudEntryMenu(app, drives, account, e, true)
            }
        }
    }
}

// The "more" menu of an entry: rename and delete, and in a grid cell download and send too.
@Kompozable
fun CloudEntryMenu(app: App, drives: Drives, account: CloudAccount, e: CloudEntry, full: Boolean) {
    var menu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var newName by remember { mutableStateOf("") }
    var confirmDelete by remember { mutableStateOf(false) }
    val targets = app.pairedPeers().filter { it.online }
    Box {
        IconButton(onClick = { menu = true }) { Icon(Icons.Filled.MoreVert, contentDescription = tr("Diğer")) }
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            if (full && !e.folder) {
                DropdownMenuItem(text = { Text(tr("İndir")) },
                                 onClick = {
                                     menu = false
                                     drives.download(account.id, e)
                                 },
                                 leadingIcon = { Icon(AppIcons.Download, contentDescription = null) })
                for (peer in targets) {
                    DropdownMenuItem(text = { Text(trf("{} cihazına gönder", peer.name)) },
                                     onClick = {
                                         menu = false
                                         drives.sendToDevice(account.id, e, peer.id)
                                     },
                                     leadingIcon = { Icon(AppIcons.forPlatform(peer.platform), contentDescription = null) })
                }
            }
            DropdownMenuItem(text = { Text(tr("Yeniden adlandır")) },
                             onClick = {
                                 menu = false
                                 newName = e.name
                                 renaming = true
                             },
                             leadingIcon = { Icon(Icons.Filled.Edit, contentDescription = null) })
            DropdownMenuItem(text = { Text(tr("Sil")) },
                             onClick = {
                                 menu = false
                                 confirmDelete = true
                             },
                             leadingIcon = { Icon(Icons.Filled.Delete, contentDescription = null) })
        }
    }
    if (renaming) {
        AlertDialog(
            onDismissRequest = { renaming = false },
            title = { Text(tr("Yeniden adlandır")) },
            text = {
                OutlinedTextField(value = newName, onValueChange = { newName = it }, singleLine = true, label = tr("Yeni ad"),
                                  modifier = Modifier.fillMaxWidth(),
                                  onSubmit = {
                                      renaming = false
                                      drives.rename(account.id, e, newName.trim())
                                  })
            },
            confirmButton = {
                TextButton(onClick = {
                    renaming = false
                    drives.rename(account.id, e, newName.trim())
                }, enabled = newName.isNotBlank()) { Text(tr("Kaydet")) }
            },
            dismissButton = { TextButton(onClick = { renaming = false }) { Text(tr("Vazgeç")) } },
        )
    }
    if (confirmDelete) {
        AlertDialog(
            onDismissRequest = { confirmDelete = false },
            title = { Text(trf("{} silinsin mi?", e.name)) },
            text = {
                Text(if (account.provider == "dropbox") tr("Dropbox'tan silinir; Dropbox silinen dosyaları bir süre geri yüklenebilir tutar.")
                     else trf("{} çöp kutusuna taşınır; oradan geri alınabilir.", providerTitle(account.provider)))
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmDelete = false
                    drives.trash(account.id, e)
                }) { Text(tr("Sil")) }
            },
            dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text(tr("Vazgeç")) } },
        )
    }
}

@Preview(widthDp = 720, heightDp = 640)
@Kompozable
fun CloudKindsPreview() {
    val account = CloudAccount(id = "gdrive:preview", provider = "gdrive", name = "Önizleme", email = "")
    val names = listOf("Rapor.pdf", "Teklif.docx", "Bütçe.xlsx", "Sunum.pptx", "Yedek.zip", "Şarkı.mp3", "main.rs", "notlar.txt",
                       "AirLink.apk", "kurulum.exe", "ubuntu.iso", "kitap.epub", "veri.bin")
    MaterialTheme {
        Column(modifier = Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            LazyVerticalGrid(GridCells.Adaptive(130.dp), modifier = Modifier.fillMaxWidth().height(400.dp),
                             verticalArrangement = Arrangement.spacedBy(8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                items(names) { n -> CloudEntryTile(appState(), drivesState(), account, CloudEntry(id = n, name = n, size = 123456)) }
            }
            CloudEntryRow(appState(), drivesState(), account, CloudEntry(id = "a", name = "Rapor.pdf", size = 123456))
            CloudEntryRow(appState(), drivesState(), account, CloudEntry(id = "b", name = "Bütçe", kind = "application/vnd.google-apps.spreadsheet"))
        }
    }
}
