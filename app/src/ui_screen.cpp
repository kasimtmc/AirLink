// AirLink's UI. Screens: this computer's screen share, a phone's screen, and phone apps in windows.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// The computer shares its screen: who is watching, and a way to end it here.
@Kompozable
fun DesktopShareBar(app: App) {
    val cs = MaterialTheme.colorScheme
    Card(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), colors = CardDefaults.cardColors(containerColor = cs.errorContainer)) {
        Column(modifier = Modifier.fillMaxWidth().padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(AppIcons.Computer, contentDescription = null, tint = cs.onErrorContainer)
                Spacer(modifier = Modifier.width(12.dp))
                Text(trf("{} bu bilgisayarı görüyor ve kontrol ediyor", app.peerName(app.desktopViewer())),
                     style = MaterialTheme.typography.bodyMedium, color = cs.onErrorContainer, modifier = Modifier.weight(1f))
                Button(onClick = { app.endDesktopShare() },
                       colors = ButtonDefaults.buttonColors(containerColor = cs.error, contentColor = cs.onError)) { Text(tr("Durdur")) }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Ekran paylaşımı sesi"), modifier = Modifier.weight(1f), color = cs.onErrorContainer)
                Switch(checked = app.screenShareAudio(), onCheckedChange = { app.setScreenShareAudio(it) })
            }
            Text(tr("Bilgisayarın varsayılan ses çıkışını gönderir."), style = MaterialTheme.typography.bodySmall,
                 color = cs.onErrorContainer)
        }
    }
}

// A paired device asks for this computer's screen and input: the user decides here.
@Kompozable
fun DesktopRequestDialog(app: App) {
    val r = app.desktopRequest()
    if (r != null) {
        val request = r!!
        AlertDialog(
            onDismissRequest = {},
            icon = { Icon(AppIcons.Computer, contentDescription = null) },
            title = { Text(trf("{} bu bilgisayarı kontrol etmek istiyor", request.peerName)) },
            text = { Text(desktopRequestText()) },
            confirmButton = { TextButton(onClick = { app.answerDesktop(true) }) { Text(tr("İzin ver")) } },
            dismissButton = { TextButton(onClick = { app.answerDesktop(false) }) { Text(tr("Reddet")) } },
        )
    }
}

// The computer's screen, fitted, with touch as a mouse: tap clicks, long press right-clicks,
// double tap double-clicks, a drag drags (left button held), two fingers scroll.
@Kompozable
fun DesktopView(app: App) {
    val frame = app.desktopFrame()
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var last by remember { mutableStateOf(Offset(0f, 0f)) }
    var typed by remember { mutableStateOf("") }
    var held by remember { mutableStateOf("") }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(app.peerName(app.desktopPeer()), style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f),
                 maxLines = 1, overflow = TextOverflow.Ellipsis)
            Button(onClick = { app.stopDesktop() }) { Text(tr("Kapat")) }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(24.dp)) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(if (app.desktopStatus() == "asking") tr("Bilgisayarda izin verin…") else tr("Görüntü bekleniyor…"), color = Color.White)
                }
            } else {
                val fw = frame.width()
                val fh = frame.height()
                Image(frame, contentDescription = tr("Bilgisayar ekranı"), contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .pointerInput(Unit) {
                              detectTapGestures(
                                  onTap = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, "")) },
                                  onDoubleTap = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, ",\"count\":2")) },
                                  onLongPress = { p -> app.desktopInput(desktopPoint("click", p, box, fw, fh, ",\"button\":\"right\"")) },
                              )
                          }
                          .pointerInput(Unit) {
                              detectTransformGestures { centroid, pan, zoom, rotation ->
                                  app.desktopInput(desktopScroll(centroid, box, fw, fh, pan))
                              }
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> last = p; app.desktopInput(desktopPoint("button", p, box, fw, fh, ",\"down\":true")) },
                                  onDrag = { change, amount -> last = last + amount; app.desktopInput(desktopPoint("move", last, box, fw, fh, "")) },
                                  onDragEnd = { app.desktopInput(desktopPoint("button", last, box, fw, fh, ",\"down\":false")) },
                                  onDragCancel = { app.desktopInput(desktopPoint("button", last, box, fw, fh, ",\"down\":false")) },
                              )
                          })
            }
        }
        FlowRow(modifier = Modifier.fillMaxWidth().padding(8.dp, 0.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            for (m in desktopModifiers()) {
                FilterChip(selected = held == m, onClick = { held = if (held == m) "" else m }, label = { Text(m) })
            }
            for (k in desktopKeys()) {
                TextButton(onClick = { app.desktopInput(desktopKey(k, held)); held = "" }) { Text(k) }
            }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true, label = tr("Bilgisayara yaz"),
                              modifier = Modifier.weight(1f))
            TextButton(onClick = {
                app.desktopInput(if (held.isNotEmpty()) desktopKey(typed, held) else desktopText(typed))
                typed = ""
                held = ""
            }, enabled = typed.isNotEmpty()) { Text(tr("Gönder")) }
        }
    }
}

// ---- A paired phone's screen (on a computer) ------------------------------------------------

// The phone's screen in a window of its own (the main window stays as it was), sized as a phone:
// its options on top, Android's three buttons under it.
@Kompozable
fun PhoneScreenWindow(app: App) {
    val peer = app.screenPeer()
    if (peer.isNotEmpty()) {
        key(peer) {
            val state = rememberWindowState(size = DpSize(420.dp, 900.dp))
            Window(onCloseRequest = { app.stopScreen() }, state = state, title = trf("{} ekranı", app.peerName(peer))) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    ScreenView(app, state)
                }
            }
        }
    }
}

@Kompozable
fun ScreenView(app: App, state: WindowState) {
    val cs = MaterialTheme.colorScheme
    val frame = app.screenFrame()
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var dragFrom by remember { mutableStateOf(Offset(0f, 0f)) }
    var dragTo by remember { mutableStateOf(Offset(0f, 0f)) }
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(start = 12.dp, end = 4.dp, top = 4.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(trf("{} ekranı", app.peerName(app.screenPeer())), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f),
                 maxLines = 1, overflow = TextOverflow.Ellipsis)
            IconButton(onClick = { state.setIsMinimized(true) }) { Icon(AppIcons.Minimize, contentDescription = tr("Küçült")) }
            IconButton(onClick = { app.stopScreen() }) { Icon(Icons.Filled.Close, contentDescription = tr("Kapat")) }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.padding(24.dp)) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(if (app.screenStatus() == "asking") tr("Telefonda ekran paylaşımına izin verin…") else tr("Görüntü bekleniyor…"), color = Color.White,
                         textAlign = TextAlign.Center)
                }
            } else {
                Image(frame, contentDescription = tr("Telefon ekranı"), contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .onPointerEvent(PointerEventType.Scroll) { e ->
                              val change = e.changes.first()
                              if (screenScroll(app, change.position, box, frame.width(), frame.height(), change.scrollDelta)) change.consume()
                          }
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
        // Android's navigation buttons.
        Row(modifier = Modifier.fillMaxWidth().background(cs.surfaceContainer).padding(vertical = 4.dp),
            horizontalArrangement = Arrangement.SpaceEvenly, verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { app.screenInput("{\"type\":\"back\"}") }) { Icon(Icons.Filled.ArrowBack, contentDescription = tr("Geri")) }
            IconButton(onClick = { app.screenInput("{\"type\":\"home\"}") }) { Icon(Icons.Filled.Home, contentDescription = tr("Ana ekran")) }
            IconButton(onClick = { app.screenInput("{\"type\":\"recents\"}") }) { Icon(AppIcons.Recents, contentDescription = tr("Son uygulamalar")) }
        }
    }
}

// ---- A paired phone's apps, each on a display of its own (advanced mode, on a computer) -----

@Kompozable
fun AppsScreen(app: App) {
    val phone = app.smsPhone()
    val apps = app.phoneApps()
    val windows = app.appWindows()
    LaunchedEffect(phone) {
        if (phone.isNotEmpty() && apps.isEmpty()) app.requestPhoneApps()
    }
    if (phone.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            EmptyState(AppIcons.Apps, tr("Telefon bağlı değil"), tr("Uygulamalar için eşleşmiş bir Android telefon çevrimiçi olmalı."))
        }
    } else {
        Column(modifier = Modifier.fillMaxSize()) {
            if (windows.isNotEmpty()) {
                FlowRow(modifier = Modifier.fillMaxWidth().padding(12.dp, 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (w in windows) {
                        TextButton(onClick = { app.showAppWindow(w.session) }) { Text(w.label) }
                    }
                }
            }
            AppGrid(app, apps)
        }
    }
}

@Kompozable
fun AppGrid(app: App, apps: List<PhoneApp>) {
    var query by remember { mutableStateOf("") }
    if (apps.isEmpty()) {
        Box(modifier = Modifier.fillMaxSize().padding(16.dp)) {
            Hint(if (app.phoneAppsLoading()) tr("Uygulamalar telefondan alınıyor…") else tr("Uygulama yok."), loading = app.phoneAppsLoading())
        }
    } else {
        val shown = if (query.isEmpty()) apps else apps.filter { matchesQuery(it.label, query) }
        Column(modifier = Modifier.fillMaxSize()) {
            Row(modifier = Modifier.fillMaxWidth().padding(16.dp, 12.dp), verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(value = query, onValueChange = { query = it }, singleLine = true, label = tr("Uygulama ara"),
                                  modifier = Modifier.weight(1f))
                Spacer(modifier = Modifier.width(8.dp))
                IconButton(onClick = { app.requestPhoneApps() }) { Icon(Icons.Default.Refresh, contentDescription = tr("Yenile")) }
            }
            Text(if (app.canMakeAppShortcuts()) trf("{} uygulama · sağ tıklayarak kısayol oluşturabilirsiniz", shown.size) else trf("{} uygulama", shown.size),
                 style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant,
                 modifier = Modifier.padding(20.dp, 0.dp))
            LazyVerticalGrid(GridCells.Adaptive(96.dp), modifier = Modifier.fillMaxSize(), contentPadding = PaddingValues(12.dp),
                             verticalArrangement = Arrangement.spacedBy(4.dp), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                items(shown) { a ->
                    // A right click: open it, or make a shortcut that opens it in its own window.
                    ContextMenuArea(items = { appMenu(app, a) }) {
                        Column(modifier = Modifier.fillMaxWidth().height(112.dp).clip(RoundedCornerShape(16.dp))
                                             .clickable(onClick = { app.openPhoneApp(a) }).padding(6.dp, 10.dp),
                               horizontalAlignment = Alignment.CenterHorizontally) {
                            if (a.icon.empty()) Box(modifier = Modifier.size(48.dp).background(MaterialTheme.colorScheme.surfaceVariant, RoundedCornerShape(12.dp)))
                            else Image(a.icon, contentDescription = null, modifier = Modifier.size(48.dp).clip(RoundedCornerShape(12.dp)))
                            Spacer(modifier = Modifier.height(8.dp))
                            Text(a.label, style = MaterialTheme.typography.bodySmall, maxLines = 2, minLines = 2, overflow = TextOverflow.Ellipsis,
                                 textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
                        }
                    }
                }
            }
        }
    }
}

@Kompozable
fun PhoneAppWindows(app: App) {
    for (w in app.appWindows()) {
        key(w.session) {
            val state = rememberWindowState(size = DpSize(400.dp, 860.dp))
            LaunchedEffect(w.frontRequests) {
                if (w.frontRequests > 0) state.toFront()
            }
            Window(onCloseRequest = { app.closeAppWindow(w.session) }, state = state, title = w.label) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    AppWindowView(app, w, state)
                }
            }
        }
    }
}

@Kompozable
fun AppWindowView(app: App, w: AppWindow, state: WindowState) {
    var box by remember { mutableStateOf(Size(0f, 0f)) }
    var last by remember { mutableStateOf(Offset(0f, 0f)) }
    var typed by remember { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    val clip = LocalClipboardManager.current
    val fw = if (w.frame.empty()) 720 else w.frame.width()
    val fh = if (w.frame.empty()) 1560 else w.frame.height()
    Column(modifier = Modifier.fillMaxSize()) {
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(w.label, style = MaterialTheme.typography.titleMedium, modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":4}") }) { Text(tr("Geri")) }
            TextButton(onClick = { state.setIsMinimized(true) }) { Text(tr("Küçült")) }
            Button(onClick = { app.closeAppWindow(w.session) }) { Text(tr("Kapat")) }
        }
        Box(modifier = Modifier.weight(1f).fillMaxWidth().background(Color(0xFF111111)), contentAlignment = Alignment.Center) {
            if (w.frame.empty()) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    CircularProgressIndicator()
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(trf("{} açılıyor…", w.label), color = Color.White)
                }
            } else {
                Image(w.frame, contentDescription = w.label, contentScale = ContentScale.Fit,
                      modifier = Modifier.fillMaxSize()
                          .onSizeChanged { box = it }
                          .onPointerEvent(PointerEventType.Scroll) { e ->
                              appScroll(app, w.session, e.changes.first().position, box, fw, fh, e.changes.first().scrollDelta)
                              e.changes.first().consume()
                          }
                          // The keyboard goes to the app while its image has the focus (a click gives it).
                          .onKeyEvent { e ->
                              if (e.type == KeyEventType.Unknown && e.text.isNotEmpty()) {
                                  app.appInput(w.session, textBody(e.text))
                                  true
                              } else if (e.type != KeyEventType.KeyDown) false
                              else if (e.isCtrlPressed && !e.isAltPressed && e.key == Key.V) {
                                  val text = clip.getText()
                                  if (text.isNotEmpty()) app.appInput(w.session, textBody(text))
                                  true
                              } else {
                                  val body = appKey(e)
                                  if (body.isNotEmpty()) app.appInput(w.session, body)
                                  body.isNotEmpty()
                              }
                          }
                          .focusRequester(focus)
                          .focusable()
                          .pointerInput(Unit) {
                              detectTapGestures(onTap = { p ->
                                  focus.requestFocus()
                                  app.appInput(w.session, appTouch("down", p, box, fw, fh))
                                  app.appInput(w.session, appTouch("up", p, box, fw, fh))
                              })
                          }
                          .pointerInput(Unit) {
                              detectDragGestures(
                                  onDragStart = { p -> focus.requestFocus(); last = p; app.appInput(w.session, appTouch("down", p, box, fw, fh)) },
                                  onDrag = { change, amount -> last = last + amount; app.appInput(w.session, appTouch("move", last, box, fw, fh)) },
                                  onDragEnd = { app.appInput(w.session, appTouch("up", last, box, fw, fh)) },
                              )
                          })
            }
        }
        Row(modifier = Modifier.fillMaxWidth().padding(8.dp), verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(value = typed, onValueChange = { typed = it }, singleLine = true, label = tr("Uygulamaya yaz"), modifier = Modifier.weight(1f))
            TextButton(onClick = { app.appInput(w.session, textBody(typed)); typed = "" }, enabled = typed.isNotEmpty()) { Text(tr("Gönder")) }
            TextButton(onClick = { app.appInput(w.session, "{\"cmd\":\"key\",\"code\":66}") }) { Text("Enter") }
        }
    }
}

// ---- Calls (a paired phone's, on a computer) ------------------------------------------------
