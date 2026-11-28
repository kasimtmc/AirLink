// AirLink's UI. Settings: this device, permissions, privacy and the features' cards.
// The @Kompozable functions are Kotlin, turned into C++ by kompozc (one module with ui.cpp and
// the other ui_*.cpp files); the helpers they share are in ui_common.hpp.

#include "ui_common.hpp"

// With a trusted device the clipboard goes to it as it changes: a copied password asks first.
@Kompozable
fun PasswordPrivacyCard(app: App) {
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(Icons.Filled.Lock, contentDescription = null, tint = cs.primary)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(tr("Şifre gizliliği"), style = MaterialTheme.typography.titleMedium)
                Text(tr("Açıkken parola yöneticisinden kopyalanan şifreler güvenilir cihazlara kendiliğinden gitmez; \"Panoda şifre var, gönderilsin mi?\" diye sorulur. Kapalıyken şifreler de kendiliğinden gider. Her iki durumda da şifreler pano geçmişinde gizli görünür ve kopyalandıktan, panel ya da uygulama kapandıktan sonra silinir."),
                     style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Switch(checked = app.passwordPrivacy(), onCheckedChange = { app.setPasswordPrivacy(it) })
        }
    }
}

// The UI's language: the system's, or Turkish or English whatever the system says.
@Kompozable
fun LanguageCard() {
    val current = languageSetting()
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Dil"), style = MaterialTheme.typography.titleSmall, modifier = Modifier.weight(1f))
            FilterChip(selected = current == "system", onClick = { setLanguageSetting("system") }, label = { Text(tr("Sistem dili")) })
            FilterChip(selected = current == "tr", onClick = { setLanguageSetting("tr") }, label = { Text("Türkçe") })
            FilterChip(selected = current == "en", onClick = { setLanguageSetting("en") }, label = { Text("English") })
        }
    }
}

@Kompozable
fun StartAtLoginCard() {
    val cs = MaterialTheme.colorScheme
    var on by remember { mutableStateOf(startAtLogin()) }
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(AppIcons.PowerSettingsNew, contentDescription = null, tint = cs.primary)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(if (isDesktop()) tr("Oturum açılınca başlat") else tr("Telefon açılınca başlat"), style = MaterialTheme.typography.titleMedium)
                Text(if (isDesktop()) tr("AirLink oturum açtığınızda pencere açmadan sistem tepsisinde başlar; cihazlarınız bu bilgisayara hemen bağlanabilir.")
                     else tr("AirLink telefon açıldığında ve güncellendiğinde uygulamayı açmanızı beklemeden arka planda başlar; bilgisayarlarınız telefona hemen bağlanabilir."),
                     style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
            }
            Spacer(modifier = Modifier.width(12.dp))
            Switch(checked = on, onCheckedChange = {
                setStartAtLogin(it)
                on = startAtLogin()
            })
        }
    }
}

@Kompozable
fun MissingPermissionsBanner(count: Int, onOpenSettings: () -> Unit) {
    val cs = MaterialTheme.colorScheme
    Card(colors = CardDefaults.cardColors(containerColor = cs.secondaryContainer), modifier = Modifier.fillMaxWidth()) {
        Row(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(if (count == 1) tr("Bir izin eksik; bazı özellikler kapalı.") else trf("{} izin eksik; bazı özellikler kapalı.", count),
                 modifier = Modifier.weight(1f), color = cs.onSecondaryContainer)
            TextButton(onClick = onOpenSettings) { Text(tr("Ayarlar")) }
        }
    }
}

// This device, its permissions and its features.
// AirLink's version, and its updates from GitHub: looked for at start and twice a day (the switch),
// or now; an available one installed as this copy was installed (update_state.hpp).
@Kompozable
fun UpdatesCard() {
    val updates = updatesState()
    val u = updates.info()
    val cs = MaterialTheme.colorScheme
    val links = uriHandler()
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(trf("AirLink {}", appVersion()), style = MaterialTheme.typography.titleSmall)
                    val line = updateLine(u)
                    if (line.isNotEmpty()) {
                        Text(line, style = MaterialTheme.typography.bodySmall,
                             color = if (u.status == "error") cs.error else if (u.status == "available") cs.primary else cs.onSurfaceVariant)
                    }
                }
                if (u.status == "available") {
                    Button(onClick = { updates.install() }) { Text(updateAction()) }
                } else {
                    OutlinedButton(onClick = { updates.check() }, enabled = !updateBusy(u)) { Text(tr("Denetle")) }
                }
            }
            if (u.status == "downloading") LinearProgressIndicator(progress = { updateFraction(u) }, modifier = Modifier.fillMaxWidth())
            if (u.status == "available" && u.page.isNotEmpty()) {
                TextButton(onClick = { links.openUri(u.page) }) { Text(tr("Sürüm notları")) }
            }
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(tr("Güncellemeleri kendiliğinden denetle"), modifier = Modifier.weight(1f))
                Switch(checked = updates.autoCheck(), onCheckedChange = { updates.setAutoCheck(it) })
            }
        }
    }
}

@Kompozable
fun SettingsScreen(app: App) {
    val paired = app.pairedPeers()
    val permissions = app.batteryRestricted() || app.canShareNotifications() || app.canShareSms() || app.canShareCalls() || app.canShareScreen() ||
                      needsBackgroundAutostartSettings()
    val phoneAudio = !isDesktop() && app.canPlayMediaAudio() && paired.any { it.online && it.platform != "android" }
    val features = phoneAudio || app.canFling() || app.canUseAdvancedMode()
    LazyColumn(
        modifier = Modifier.fillMaxSize().wrapContentWidth().widthIn(0.dp, 760.dp),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        item { ThisDeviceCard(app.me()) }
        item { SectionTitle(tr("Genel")) }
        item { LanguageCard() }
        item { UpdatesCard() }
        if (canStartAtLogin()) item { StartAtLoginCard() }
        if (permissions) {
            item { SectionTitle(tr("İzinler")) }
            if (app.batteryRestricted()) item { BatteryRestrictionCard() }
            if (needsBackgroundAutostartSettings()) item { BackgroundAutostartCard() }
            if (app.canShareNotifications()) item { NotificationAccessCard(app) }
            if (app.canShareSms()) item { SmsAccessCard(app) }
            if (app.canShareCalls()) item { CallAccessCard(app) }
            if (app.canShareScreen()) item { ScreenControlCard(app) }
        }
        if (isDesktop() && drivesState().available()) {
            item { SectionTitle(tr("Bulut sürücüleri")) }
            item { CloudAccountsCard(drivesState()) }
        }
        if (paired.any { it.autoAccept }) {
            item { SectionTitle(tr("Gizlilik")) }
            item { PasswordPrivacyCard(app) }
        }
        if (features) {
            item { SectionTitle(tr("Özellikler")) }
            if (phoneAudio) item { PhoneAudioCard(app) }
            if (app.canFling()) item { FlingCard(app) }
            if (app.canUseAdvancedMode()) item { AdvancedModeCard(app) }
        }
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
                Text(trf("Bu cihaz · {} · port {} · sürüm {}", shortId(me.id), me.port, appVersion()), color = cs.onPrimaryContainer)
                Text(trf("Gelen dosyalar: {}", me.downloadDir), style = MaterialTheme.typography.bodySmall,
                     color = cs.onPrimaryContainer, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}

@Kompozable
fun BatteryRestrictionCard() {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Pil kısıtlaması"), style = MaterialTheme.typography.titleMedium)
            Text(tr("Android pil tasarrufu için AirLink'i arka planda yavaşlatabilir ya da durdurabilir; ekran kapalıyken dosya, pano ve aramalar gecikebilir. AirLink'in kısıtlamasız çalışmasına izin verin."),
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            FilledTonalButton(onClick = { requestBatteryExemption() }) { Text(tr("Kısıtlamayı kaldır")) }
        }
    }
}

@Kompozable
fun BackgroundAutostartCard() {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Arka planda bağlı kal"), style = MaterialTheme.typography.titleMedium)
            Text(tr("Xiaomi, uygulama süreci kapandığında AirLink'in yeniden başlamasını engelleyebilir. Ayarlar'da AirLink için arka planda otomatik başlatmayı açın."),
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            FilledTonalButton(onClick = { openBackgroundAutostartSettings() }) { Text(tr("Otomatik başlatma ayarları")) }
        }
    }
}

@Kompozable
fun NotificationAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Bildirimleri bilgisayarda göster"), style = MaterialTheme.typography.titleMedium)
            if (app.notificationAccess()) {
                Text(tr("Bu telefonun bildirimleri eşleşmiş bilgisayarlara gidiyor; oradan kapatılabilir ya da yanıtlanabilir."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(tr("Bilgisayarlarınız bu telefonun bildirimlerini görsün ve mesajları oradan yanıtlayın. Ayarlar'da AirLink'e bildirim erişimi verin."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openNotificationAccessSettings() }) { Text(tr("İzin ver")) }
            }
        }
    }
}

@Kompozable
fun SmsAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("SMS'leri bilgisayardan kullan", style = MaterialTheme.typography.titleMedium)
            if (app.smsAccess()) {
                Text(tr("Eşleşmiş bilgisayarlar bu telefonun SMS'lerini görebilir ve gönderebilir."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(tr("Bilgisayarınızdan mesajlarınızı okuyun ve SMS gönderin. AirLink'e SMS (ve isimler için kişiler) izni verin."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestSmsAccess() }) { Text(tr("İzin ver")) }
            }
        }
    }
}

@Kompozable
fun CallAccessCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Aramaları bilgisayarda göster"), style = MaterialTheme.typography.titleMedium)
            if (app.callAccess()) {
                Text(tr("Telefon çalınca eşleşmiş bilgisayarlar arayanı görür; oradan yanıtlanabilir, reddedilebilir ya da numara aratılabilir."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(tr("Kim aradığını bilgisayarda görün, aramayı oradan reddedin ve arama kaydınıza ulaşın. AirLink'e telefon ve arama izinleri verin."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.requestCallAccess() }) { Text(tr("İzin ver")) }
            }
        }
    }
}

@Kompozable
fun ScreenControlCard(app: App) {
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Ekranı bilgisayardan kontrol et"), style = MaterialTheme.typography.titleMedium)
            if (app.screenControl()) {
                Text(tr("Bilgisayar ekranınızı görmek istediğinde onayınız sorulur; paylaşım sürerken oradan dokunabilir ve kaydırabilir."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                Text(tr("Ekranınız bilgisayarda görünebilir (her seferinde onayınızla). Oradan dokunmak için Erişilebilirlik'te \"AirLink ekran kontrolü\"nü açın."),
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                FilledTonalButton(onClick = { app.openScreenControlSettings() }) { Text(tr("Erişilebilirliği aç")) }
            }
        }
    }
}

@Kompozable
fun PhoneAudioCard(app: App) {
    val computers = app.pairedPeers().filter { it.online && it.platform != "android" }
    val target = if (app.mediaAudioTarget().isEmpty()) app.flingTarget() else app.mediaAudioTarget()
    val bluetoothTarget = computers.any { it.id == target && (it.platform == "linux" || it.platform == "windows") }
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Telefon sesini paylaş"), style = MaterialTheme.typography.titleMedium)
            Text(tr("Hedef bilgisayar"), style = MaterialTheme.typography.bodySmall)
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (computer in computers) {
                    FilterChip(selected = target == computer.id, onClick = { app.setMediaAudioTarget(computer.id) },
                               label = { Text(computer.name) })
                }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Otomatik bağlan"), modifier = Modifier.weight(1f))
                Switch(checked = app.audioAutoConnect(), onCheckedChange = { app.setAudioAutoConnect(it) })
            }
            if (app.audioAutoConnect()) {
                Text(tr("Arama sesi açıksa görüşme başladığında Bluetooth bağlantısı kurulur."),
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            if (bluetoothTarget) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(tr("Arama sesini paylaş"), modifier = Modifier.weight(1f))
                    Switch(checked = app.callAudioOn(), onCheckedChange = { if (it) app.startCallAudio() else app.stopCallAudio() })
                }
                if (app.callAudioOn() && !app.audioAutoConnect()) {
                    OutlinedButton(onClick = { app.shareCurrentCallAudio() }) { Text(tr("Süren görüşmenin sesini aktar")) }
                }
            } else {
                Text(tr("Arama sesi paylaşımı Linux ve Windows bilgisayarda kullanılabilir."),
                     style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Ekran paylaşımı sesi"), modifier = Modifier.weight(1f))
                Switch(checked = app.screenShareAudio(), onCheckedChange = { app.setScreenShareAudio(it) })
            }
            Text(tr("Tek uygulama ve tam ekran paylaşımında telefonun medya çıkışını gönderir."), style = MaterialTheme.typography.bodySmall,
                 color = MaterialTheme.colorScheme.onSurfaceVariant)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Müzik oynatılırken ekran sesini durdur"), modifier = Modifier.weight(1f))
                Switch(checked = app.pauseScreenAudioDuringMusic(), onCheckedChange = { app.setPauseScreenAudioDuringMusic(it) })
            }
            if (app.pauseScreenAudioDuringMusic() && !app.notificationAccess() && !app.mediaAudioEnabled()) {
                OutlinedButton(onClick = { app.openNotificationAccessSettings() }) { Text(tr("Oynatmayı algılamak için bildirim erişimini aç")) }
            }
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(tr("Oynatıcı sesini aktar"), modifier = Modifier.weight(1f))
                Switch(checked = app.mediaAudioEnabled(), enabled = bluetoothTarget,
                       onCheckedChange = { app.setMediaAudioEnabled(it) })
            }
            if (app.mediaAudioEnabled()) {
                Text(app.mediaAudioStatus(), style = MaterialTheme.typography.bodySmall,
                     color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (!app.notificationAccess()) {
                    OutlinedButton(onClick = { app.openNotificationAccessSettings() }) { Text(tr("Bildirim erişimini aç")) }
                }
            }
            OutlinedButton(onClick = { app.chooseMediaAudioPlayers() }) { Text(tr("Oynatıcıları seç")) }
            Text(tr("Seçili bir oynatıcı çalarken eşleşmiş bilgisayara Bluetooth medya sesi gider."),
                 style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

// ---- The tray panel (a click on the tray icon) -------------------------------------------------

// Fling to send (phase 4B): push the phone toward the computer.
@Kompozable
fun FlingCard(app: App) {
    val target = app.flingTarget()
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.fillMaxWidth().padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(tr("Fırlatarak gönder"), style = MaterialTheme.typography.titleMedium)
                    Text(tr("Telefonu bilgisayara doğru kısa ve hızlı bir hareketle itin. AirLink açıkken paylaşılan dosyalar ya da kopyalanan metin, başka bir uygulama açıkken o uygulamanın gösterdiği içerik gider: galerideki fotoğraf ya da video, galeri ve dosya yöneticisinde seçili dosyalar, çalan müzik."),
                         style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
                    Text(if (target.isEmpty()) tr("Hedef: çevrimiçi bilgisayar yok") else trf("Hedef: {}", app.peerName(target)),
                         style = MaterialTheme.typography.labelMedium)
                }
                Spacer(modifier = Modifier.width(12.dp))
                Switch(checked = app.flingEnabled(), onCheckedChange = { app.setFlingEnabled(it) })
            }
            if (app.flingEnabled() && !app.screenControl()) {
                Text(tr("Diğer uygulamalardan göndermek için Erişilebilirlik'te \"AirLink ekran kontrolü\" açık olmalı."),
                     style = MaterialTheme.typography.bodySmall, color = cs.primary)
                OutlinedButton(onClick = { app.openScreenControlSettings() }) { Text(tr("Erişilebilirliği aç")) }
            }
            if (app.flingEnabled() && !app.flingMediaAccess()) {
                Text(tr("Galerideki fotoğrafı doğrudan bulmak için fotoğraf, video ve müzik erişimi gerekir."),
                     style = MaterialTheme.typography.bodySmall, color = cs.primary)
                OutlinedButton(onClick = { app.requestFlingMediaAccess() }) { Text(tr("Medya erişimi ver")) }
            }
        }
    }
}

@Kompozable
fun AdvancedModeCard(app: App) {
    val cs = MaterialTheme.colorScheme
    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(tr("Gelişmiş mod"), style = MaterialTheme.typography.titleMedium)
            Text(tr("Uygulamaları bilgisayarda kendi pencerelerinde açın; gerçek dokunuş ve klavye. Kablosuz hata ayıklama ile bir kez kurulur."),
                 color = cs.onSurfaceVariant)
            if (app.advancedStatus().isNotEmpty()) {
                Text(app.advancedStatus(), style = MaterialTheme.typography.bodySmall,
                     color = if (app.advancedReady()) Color(0xFF2E7D32) else cs.primary)
            }
            OutlinedTextField(value = app.advancedPort(), onValueChange = { app.setAdvancedPort(it) },
                              singleLine = true, label = tr("Kablosuz hata ayıklama portu (isteğe bağlı)"),
                              modifier = Modifier.fillMaxWidth())
            Text(tr("Bağlantı bulunamazsa Android > Kablosuz hata ayıklama ekranındaki IP:port değerinin yalnızca portunu girin."),
                 style = MaterialTheme.typography.bodySmall, color = cs.onSurfaceVariant)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = { app.startAdvancedSetup() }) { Text(if (app.advancedReady()) tr("Yeniden kur") else tr("Kur")) }
                TextButton(onClick = { app.advancedTest() }) { Text(tr("Dene")) }
            }
        }
    }
}

// ---- A paired computer's screen (on a phone, phase 4) ----------------------------------------
