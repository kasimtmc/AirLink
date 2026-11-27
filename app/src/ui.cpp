// AirLink's UI: the app, its tray icon, the home screen with its tabs, and the small pieces every
// screen uses. The @Kompozable functions are Kotlin, turned into C++ by kompozc; the screens are in
// ui_*.cpp (one module with this file), the C++ helpers they share in ui_common.hpp.

#include "ui_common.hpp"

@Kompozable
fun AirLinkApp() {
    val dark = isSystemInDarkTheme()
    val scheme = if (isDynamicColorAvailable()) {
        if (dark) dynamicDarkColorScheme() else dynamicLightColorScheme()
    } else {
        if (dark) darkColorScheme() else lightColorScheme()
    }
    // Started at login (or by a phone app's shortcut): straight to the tray, the window shown from
    // there. The tray icon is registered a moment after start: waited for (up to 3 s).
    LaunchedEffect(Unit) {
        var waited = 0
        while (launchedAtLogin() && waited < 60) {
            if (isTraySupported()) {
                setAppWindowVisible(false)
                waited = 60
            } else {
                delay(50)
                waited += 1
            }
        }
    }
    MaterialTheme(colorScheme = scheme) {
        Home()
        if (isDesktop()) PhoneAppWindows(appState())
        if (isDesktop()) PhoneScreenWindow(appState())
        if (isDesktop()) AirLinkTray(appState())
    }
}

// The tray icon on a computer: AirLink goes on in the background when its window is closed (it
// keeps the device reachable), and comes back from here.
@Kompozable
fun AirLinkTray(app: App) {
    val clipboard = clipboardManager()
    val paused = app.sharingPaused()
    val online = app.pairedPeers().filter { it.online }
    val status = if (paused) TrayIconStatus.Paused else if (app.startError().isNotEmpty() || app.recentError()) TrayIconStatus.Error
                 else if (!app.ready()) TrayIconStatus.Starting else if (online.isEmpty()) TrayIconStatus.NoDevices
                 else TrayIconStatus.Connected
    // Something came (files, clipboard text): the icon blinks purple a few times.
    val received = app.receivedCount()
    var flash by remember { mutableStateOf(false) }
    LaunchedEffect(received) {
        if (received > 0) {
            for (i in 0 until 6) {
                flash = !flash
                delay(350)
            }
            flash = false
        }
    }
    val trayState = rememberTrayState()
    LaunchedEffect(received) { showTrayNotifications(trayState) }
    val shown = if (flash) TrayIconStatus.Received else status
    val icon = remember(shown) { statusTrayIcon(shown) }
    val tip = if (paused) tr("AirLink · paylaşım duraklatıldı") else if (status == TrayIconStatus.Error) tr("AirLink · işlem hatası")
              else if (status == TrayIconStatus.Starting) tr("AirLink · başlatılıyor")
              else if (online.isEmpty()) tr("AirLink · bağlı cihaz yok") else trf("AirLink · {} cihaz bağlı", online.size)
    // The panel widget (Plasma) colors its icon as this one.
    val statusName = when (status) {
        TrayIconStatus.Connected -> "connected"
        TrayIconStatus.NoDevices -> "nodevices"
        TrayIconStatus.Paused -> "paused"
        TrayIconStatus.Error -> "error"
        else -> "starting"
    }
    LaunchedEffect(statusName, tip, received) { publishStatus(statusName, tip, received) }
    // A click on the icon opens the panel there; a click while it is open closes it. Where the click
    // takes the focus from the panel, the panel has closed just before the click comes: a click
    // right after it closed is that one, and leaves it closed.
    var panel by remember { mutableStateOf<WindowPopup?>(null) }
    var panelClosedAt by remember { mutableStateOf(0L) }
    val togglePanel = { at: IntOffset? ->
        if (panel != null) {
            panel = null
            panelClosedAt = nowMs()
        } else if (nowMs() - panelClosedAt > 400) {
            panel = WindowPopup(anchor = at)
        }
    }
    Tray(icon = icon, tooltip = tip, onAction = { at -> togglePanel(at) }, state = trayState) {
        Item(tr("AirLink'i aç"), onClick = { setAppWindowVisible(true) })
        val paired = app.pairedPeers()
        if (paired.isNotEmpty()) {
            Separator()
            for (peer in paired) {
                Menu(peer.name, enabled = app.ready() && !paused && peer.online) {
                    Item(tr("Dosya gönder…"), onClick = { app.pickAndSend(peer.id) })
                    Item(tr("Panoyu gönder"), onClick = { app.sendClipboard(peer.id, clipboard.getText()) })
                    if (peer.platform == "android") {
                        Separator()
                        Item(tr("Ekranı göster"), onClick = { app.startScreen(peer.id) })
                    }
                }
            }
        }
        Separator()
        Item(tr("Gelen dosyaları aç"), onClick = { openDownloads(app.me().downloadDir) })
        Item(if (paused) tr("Paylaşımı sürdür") else if (!app.ready()) tr("Bağlantıyı yeniden dene") else tr("Paylaşımı duraklat"),
             onClick = { app.setSharingPaused(!paused && app.ready()) })
        Separator()
        Item(tr("Çık"), onClick = { exitApplication() })
    }
    // The Plasma panel widget's click (`airlink --panel X Y`): the panel at that point, as a tray click.
    val request = app.panelRequest()
    LaunchedEffect(request.serial) {
        if (request.serial > 0) togglePanel(if (request.x == 0 && request.y == 0) null else IntOffset(request.x, request.y))
    }
    val open = panel
    if (open != null) {
        Window(onCloseRequest = {
                   panel = null
                   panelClosedAt = nowMs()
               }, popup = open!!, transparent = true, title = "AirLink", state = rememberWindowState(size = DpSize(380.dp, 600.dp))) {
            // Rounded: the window is see-through outside this surface (opaque and square where the
            // platform can't have it).
            Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceContainer, modifier = Modifier.fillMaxSize()) {
                TrayPanel(app, onOpenApp = {
                    panel = null
                    panelClosedAt = nowMs()
                    setAppWindowVisible(true)
                })
            }
        }
    }
}

// Home's destinations.
constexpr int TAB_DEVICES = 0, TAB_MESSAGES = 1, TAB_PHONE = 2, TAB_APPS = 3, TAB_SETTINGS = 4, TAB_CLIPBOARD = 5, TAB_FILES = 6;
constexpr int TAB_CLOUD = 100;  // a cloud drive account's tab: TAB_CLOUD + its index

static std::string tabTitle(int tab) {
    switch (tab) {
        case TAB_MESSAGES: return tr("Mesajlar");
        case TAB_PHONE: return tr("Telefon");
        case TAB_APPS: return tr("Uygulamalar");
        case TAB_SETTINGS: return tr("Ayarlar");
        case TAB_CLIPBOARD: return tr("Pano");
        case TAB_FILES: return tr("Dosyalar");
        default: return "AirLink";
    }
}

@Kompozable
fun Home() {
    val app = appState()
    val snackbar = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()
    val clipboard = clipboardManager()
    var tab by remember { mutableStateOf(0) }

    val clipEntries = systemClipboard()
    // Received files and text notify only while AirLink's window isn't the focused one.
    val focused = LocalWindowInfo.current.isWindowFocused
    LaunchedEffect(focused) { setAppForeground(focused) }

    // The core's events, applied on the UI thread.
    LaunchedEffect(Unit) {
        var clipboardTicks = 0
        while (true) {
            app.pump()
            for (text in app.takeIncomingClipboard()) putOnClipboard(clipboard, text, app.incomingClipboardSecret())
            if (++clipboardTicks % 20 == 0) app.syncClipboard(clipboardText(clipboard), clipboardSensitive(clipEntries))
            for (message in app.takeNotices()) scope.launch { snackbar.showSnackbar(message) }
            delay(50)
        }
    }

    // Destinations: a computer also has a paired phone's messages, calls and apps.
    val desktopTabs = isDesktop() && app.ready()
    val drives = drivesState()
    val cloudAccounts = if (desktopTabs) drives.accounts() else listOf()
    // (a tab whose account was just removed goes back to Settings, where it was removed)
    if (tab >= TAB_CLOUD && tab - TAB_CLOUD >= cloudAccounts.size) tab = TAB_SETTINGS
    val cloudIndex = if (tab >= TAB_CLOUD) tab - TAB_CLOUD else -1
    val activeCall = app.activeCall()
    var minimizedCall by remember { mutableStateOf(0L) }
    val callOpen = activeCall != null && isDesktop() && minimizedCall != activeCall!!.since
    // (a phone's screen opens in a window of its own on a computer: PhoneScreenWindow)
    val fullView = app.ready() && (app.desktopPeer().isNotEmpty() || (!isDesktop() && app.screenPeer().isNotEmpty()) || callOpen)
    if (!desktopTabs && tab != TAB_SETTINGS && tab != TAB_DEVICES && tab != TAB_CLIPBOARD && tab != TAB_FILES) tab = TAB_DEVICES
    val navType = if (!app.ready() || fullView) NavigationSuiteType.None
                  else NavigationSuiteScaffoldDefaults.navigationSuiteType(currentWindowAdaptiveInfo())

    NavigationSuiteScaffold(
        navigationItems = {
            NavigationSuiteItem(selected = tab == TAB_DEVICES, onClick = { tab = TAB_DEVICES },
                                icon = { Icon(AppIcons.Devices, contentDescription = null) }, label = { Text(tr("Cihazlar")) })
            NavigationSuiteItem(selected = tab == TAB_FILES, onClick = { tab = TAB_FILES },
                                icon = { Icon(AppIcons.Folder, contentDescription = null) }, label = { Text(tr("Dosyalar")) })
            NavigationSuiteItem(selected = tab == TAB_CLIPBOARD, onClick = { tab = TAB_CLIPBOARD },
                                icon = { Icon(AppIcons.ContentPaste, contentDescription = null) }, label = { Text(tr("Pano")) })
            if (desktopTabs) {
                NavigationSuiteItem(selected = tab == TAB_MESSAGES, onClick = { tab = TAB_MESSAGES },
                                    icon = { Icon(AppIcons.Sms, contentDescription = null) }, label = { Text(tr("Mesajlar")) })
                NavigationSuiteItem(selected = tab == TAB_PHONE, onClick = { tab = TAB_PHONE },
                                    icon = { Icon(AppIcons.Phone, contentDescription = null) }, label = { Text(tr("Telefon")) })
                NavigationSuiteItem(selected = tab == TAB_APPS, onClick = { tab = TAB_APPS },
                                    icon = { Icon(AppIcons.Apps, contentDescription = null) }, label = { Text(tr("Uygulamalar")) })
            }
            cloudAccounts.forEachIndexed { i, a ->
                NavigationSuiteItem(selected = tab == TAB_CLOUD + i, onClick = { tab = TAB_CLOUD + i },
                                    icon = { Icon(AppIcons.Cloud, contentDescription = null) }, label = { Text(cloudTabLabel(cloudAccounts, a)) })
            }
            NavigationSuiteItem(selected = tab == TAB_SETTINGS, onClick = { tab = TAB_SETTINGS },
                                icon = { Icon(Icons.Filled.Settings, contentDescription = null) }, label = { Text(tr("Ayarlar")) })
        },
        navigationSuiteType = navType,
        modifier = Modifier.fillMaxSize(),
    ) {
        Scaffold(
            topBar = {
                if (!callOpen) TopAppBar(
                    title = { Text(if (cloudIndex >= 0) cloudTitle(cloudAccounts[cloudIndex]) else tabTitle(tab)) },
                    actions = {
                        if (tab == TAB_DEVICES && app.transfers().any { !it.active() }) {
                            TextButton(onClick = { app.clearFinishedTransfers() }) { Text(tr("Temizle")) }
                        }
                        if (tab == TAB_FILES && app.fileHistory().isNotEmpty()) {
                            TextButton(onClick = { app.clearFileHistory() }) { Text(tr("Geçmişi temizle")) }
                        }
                        if (tab == TAB_CLIPBOARD && app.clipboardHistory().isNotEmpty()) {
                            TextButton(onClick = { app.clearClipboardHistory() }) { Text(tr("Temizle")) }
                        }
                    },
                )
            },
            snackbarHost = { SnackbarHost(snackbar) },
        ) { padding ->
            Column(modifier = Modifier.fillMaxSize().padding(padding)) {
                if (activeCall != null && isDesktop() && !callOpen) ActiveCallBar(app, activeCall!!, onOpen = { minimizedCall = 0L })
                if (app.desktopViewer().isNotEmpty()) DesktopShareBar(app)
                Box(modifier = Modifier.fillMaxSize()) {
                    if (callOpen) {
                        CallScreen(app, activeCall!!, onMinimize = { minimizedCall = activeCall!!.since })
                    } else if (app.ready() && app.desktopPeer().isNotEmpty()) {
                        DesktopView(app)
                    } else if (app.ready()) {
                        if (cloudIndex >= 0) CloudScreen(app, drives, cloudAccounts[cloudIndex])
                        else if (tab == TAB_MESSAGES) MessagesScreen(app)
                        else if (tab == TAB_PHONE) PhoneScreen(app)
                        else if (tab == TAB_APPS) AppsScreen(app)
                        else if (tab == TAB_SETTINGS) SettingsScreen(app)
                        else if (tab == TAB_CLIPBOARD) ClipboardScreen(app, clipboard)
                        else if (tab == TAB_FILES) FilesScreen(app)
                        else DeviceList(app, clipboard, onOpenSettings = { tab = TAB_SETTINGS })
                    } else if (app.sharingPaused()) {
                        Column(modifier = Modifier.align(Alignment.Center), horizontalAlignment = Alignment.CenterHorizontally) {
                            Text(tr("Paylaşım duraklatıldı"))
                            TextButton(onClick = { app.setSharingPaused(false) }) { Text(tr("Paylaşımı sürdür")) }
                        }
                    } else if (app.startError().isNotEmpty()) {
                        Text(trf("Başlatılamadı:\n{}", app.startError()), modifier = Modifier.align(Alignment.Center).padding(24.dp))
                    } else {
                        CircularProgressIndicator(modifier = Modifier.align(Alignment.Center))
                    }
                }
            }
        }
    }

    // Password privacy: a copied password goes to trusted devices only after a yes.
    if (app.passwordPending()) {
        AlertDialog(
            onDismissRequest = { app.answerPasswordSend(false) },
            icon = { Icon(Icons.Filled.Lock, contentDescription = null) },
            title = { Text(tr("Panoda şifre var")) },
            text = { Text(tr("Kopyaladığınız şifre güvenilir cihazlarınıza gönderilsin mi?")) },
            confirmButton = { TextButton(onClick = { app.answerPasswordSend(true) }) { Text(tr("Gönder")) } },
            dismissButton = { TextButton(onClick = { app.answerPasswordSend(false) }) { Text(tr("Gönderme")) } },
        )
    }
    IncomingPrompt(app)
    OutgoingPairingDialog(app)
    ShareDialog(app)
    IncomingCallDialog(app)
    DesktopRequestDialog(app)
}

@Kompozable
fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(top = 16.dp, bottom = 4.dp))
}

// A screen with nothing to show yet: an icon, what is missing and what to do, in the middle.
@Kompozable
fun EmptyState(icon: ImageVector, title: String, text: String) {
    val cs = MaterialTheme.colorScheme
    Column(modifier = Modifier.fillMaxSize().padding(32.dp), horizontalAlignment = Alignment.CenterHorizontally,
           verticalArrangement = Arrangement.Center) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(48.dp), tint = cs.primary)
        Spacer(modifier = Modifier.height(16.dp))
        Text(title, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
        Spacer(modifier = Modifier.height(4.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium, color = cs.onSurfaceVariant, textAlign = TextAlign.Center)
    }
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

// The phone's battery under its name: green while charging, red when low.
@Kompozable
fun BatteryLine(battery: PhoneBattery) {
    val cs = MaterialTheme.colorScheme
    val tint = if (battery.charging) Color(0xFF2E7D32) else if (battery.level <= 15) cs.error else cs.onSurfaceVariant
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(if (battery.charging) AppIcons.BatteryCharging else AppIcons.Battery, contentDescription = null, tint = tint,
             modifier = Modifier.size(14.dp))
        Spacer(modifier = Modifier.width(2.dp))
        Text(batteryText(battery), style = MaterialTheme.typography.bodySmall, color = tint)
    }
}

kompoz::Application kompoz::createApplication(int argc, char** argv) {
#if defined(_WIN32)
    // A GUI-subsystem process has no console in WinBoat's app view. Keep startup diagnostics
    // when requested by the bundled diagnostic launcher, including Kompoz's GPU errors.
    if (const char* path = std::getenv("AIRLINK_DIAG_LOG")) {
        if (std::freopen(path, "w", stderr)) {
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::fprintf(stderr, "airlink: diagnostic launch\n");
        }
    }
#endif
    // `airlink --send FILE...`: to the running instance if there is one (this process then exits).
    airlink::platform::handleLaunch(argc, argv);
    // Create the app's states here, outside any composition's snapshot.
    App::get();
    // On a computer, closing the window keeps AirLink running in the tray (where there is one).
    // Wide enough for the side rail and a readable list (Kompoz's default is phone-sized).
    return {.title = "AirLink", .width = 880, .height = 820, .content = [] { AirLinkApp(); }, .onCloseRequest = [] {
                App::get().dropPasswords();  // (the window closes: the clipboard history's passwords go)
                if (kompoz::isTraySupported()) kompoz::setAppWindowVisible(false);
                else kompoz::exitApplication();
            }};
}
