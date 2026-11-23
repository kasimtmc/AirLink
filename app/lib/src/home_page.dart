import 'dart:async';
import 'dart:io';

import 'package:desktop_drop/desktop_drop.dart';
import 'package:file_picker/file_picker.dart';
import 'package:material_ui/material_ui.dart';

import 'controller.dart';
import 'rust/api/airlink.dart';

class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.controller});
  final AirLinkController controller;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  AirLinkController get c => widget.controller;
  final _subs = <StreamSubscription>[];

  @override
  void initState() {
    super.initState();
    _subs.add(c.prompts.listen(_showPrompt));
    _subs.add(c.notices.listen(_toast));
  }

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    super.dispose();
  }

  void _toast(String msg) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(content: Text(msg), behavior: SnackBarBehavior.floating),
      );
  }

  Future<void> _showPrompt(Prompt prompt) async {
    final accept = await showDialog<bool>(
      context: context,
      barrierDismissible: false,
      builder: (ctx) => switch (prompt) {
        PairingPrompt(:final peerName, :final code) => AlertDialog(
          icon: const Icon(Icons.link),
          title: Text('$peerName eşleşmek istiyor'),
          content: _CodeView(
            code: code,
            hint: 'Diğer cihazda da aynı kod görünüyorsa onaylayın.',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.pop(ctx, false),
              child: const Text('Reddet'),
            ),
            FilledButton(
              onPressed: () => Navigator.pop(ctx, true),
              child: const Text('Kodlar aynı'),
            ),
          ],
        ),
        FileOfferPrompt(:final peerName, :final files, :final totalSize) =>
          AlertDialog(
            icon: const Icon(Icons.download),
            title: Text('$peerName dosya göndermek istiyor'),
            content: SizedBox(
              width: 420,
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('${files.length} dosya, toplam ${humanSize(totalSize)}'),
                  const SizedBox(height: 12),
                  ConstrainedBox(
                    constraints: const BoxConstraints(maxHeight: 240),
                    child: ListView(
                      shrinkWrap: true,
                      children: [
                        for (final f in files)
                          ListTile(
                            dense: true,
                            contentPadding: EdgeInsets.zero,
                            leading: const Icon(
                              Icons.insert_drive_file_outlined,
                            ),
                            title: Text(
                              f.path,
                              overflow: TextOverflow.ellipsis,
                            ),
                            trailing: Text(humanSize(f.size)),
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(ctx, false),
                child: const Text('Reddet'),
              ),
              FilledButton(
                onPressed: () => Navigator.pop(ctx, true),
                child: const Text('Kabul et'),
              ),
            ],
          ),
      },
    );
    c.respond(prompt.requestId, accept ?? false);
  }

  Future<void> _pair(PeerDto peer) async {
    try {
      final msg = await c.startPairing(peer.id, (code) async {
        final ok = await showDialog<bool>(
          context: context,
          barrierDismissible: false,
          builder: (ctx) => AlertDialog(
            icon: const Icon(Icons.link),
            title: Text('${peer.name} ile eşleş'),
            content: _CodeView(
              code: code,
              hint: 'Bu kod diğer cihazda da görünüyor mu?',
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(ctx, false),
                child: const Text('İptal'),
              ),
              FilledButton(
                onPressed: () => Navigator.pop(ctx, true),
                child: const Text('Kodlar aynı'),
              ),
            ],
          ),
        );
        if (ok == true) _toast('Diğer cihazın onayı bekleniyor…');
        return ok ?? false;
      });
      _toast(msg);
    } catch (e) {
      _toast('Eşleşme başarısız: $e');
    }
  }

  Future<void> _pickAndSend(PeerDto peer) async {
    final picked = await FilePicker.pickFiles(
      dialogTitle: '${peer.name} cihazına gönder',
    );
    final paths = picked.map((f) => f.path).whereType<String>().toList();
    if (paths.isNotEmpty) await c.sendFiles(peer.id, paths);
  }

  Future<void> _confirmForget(PeerDto peer) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('${peer.name} unutulsun mu?'),
        content: const Text(
          'Tekrar dosya paylaşmak için yeniden eşleşmeniz gerekecek.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Vazgeç'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Unut'),
          ),
        ],
      ),
    );
    if (ok == true) c.forget(peer.id);
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: c,
      builder: (context, _) {
        final theme = Theme.of(context);
        return Scaffold(
          appBar: AppBar(
            title: const Text('AirLink'),
            actions: [
              if (c.transfers.values.any((t) => !t.active))
                IconButton(
                  tooltip: 'Bitenleri temizle',
                  icon: const Icon(Icons.clear_all),
                  onPressed: c.clearFinishedTransfers,
                ),
            ],
          ),
          body: !c.ready
              ? Center(
                  child: c.startError == null
                      ? const CircularProgressIndicator()
                      : Padding(
                          padding: const EdgeInsets.all(24),
                          child: Text(
                            'Başlatılamadı:\n${c.startError}',
                            textAlign: TextAlign.center,
                          ),
                        ),
                )
              : ListView(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
                  children: [
                    _ThisDeviceCard(me: c.me!, downloadDir: c.downloadDir),
                    if (c.transfers.isNotEmpty) ...[
                      _SectionTitle('Aktarımlar'),
                      for (final t in c.transfers.values.toList().reversed)
                        _TransferTile(
                          transfer: t,
                          peerName: c.peerName(t.peerId),
                        ),
                    ],
                    _SectionTitle('Eşleşmiş cihazlar'),
                    if (c.paired.isEmpty)
                      _Empty(
                        'Henüz eşleşmiş cihaz yok. Aşağıdan yakındaki bir cihazla eşleşin.',
                      )
                    else
                      for (final p in c.paired)
                        _PairedPeerCard(
                          peer: p,
                          onSendFiles: () => _pickAndSend(p),
                          onDropFiles: (paths) => c.sendFiles(p.id, paths),
                          onSendClipboard: () => c.sendClipboard(p.id),
                          onForget: () => _confirmForget(p),
                        ),
                    _SectionTitle('Yakındaki cihazlar'),
                    if (c.nearby.isEmpty)
                      _Empty(
                        'Aranıyor… Diğer cihazda AirLink açık ve aynı ağda olmalı.',
                        loading: true,
                      )
                    else
                      for (final p in c.nearby)
                        Card.outlined(
                          child: ListTile(
                            leading: Icon(
                              platformIcon(p.platform),
                              color: theme.colorScheme.primary,
                            ),
                            title: Text(p.name),
                            subtitle: Text(
                              '${p.platform} · ${p.id.substring(0, 8)}',
                            ),
                            trailing: FilledButton.tonalIcon(
                              onPressed: () => _pair(p),
                              icon: const Icon(Icons.link),
                              label: const Text('Eşleş'),
                            ),
                          ),
                        ),
                  ],
                ),
        );
      },
    );
  }
}

IconData platformIcon(String platform) => switch (platform) {
  'android' => Icons.smartphone,
  'windows' => Icons.desktop_windows,
  'macos' => Icons.laptop_mac,
  _ => Icons.computer,
};

class _ThisDeviceCard extends StatelessWidget {
  const _ThisDeviceCard({required this.me, required this.downloadDir});
  final DeviceInfoDto me;
  final String downloadDir;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Card(
      color: scheme.primaryContainer,
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            CircleAvatar(
              radius: 26,
              backgroundColor: scheme.primary,
              child: Icon(platformIcon(me.platform), color: scheme.onPrimary),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(me.name, style: Theme.of(context).textTheme.titleLarge),
                  Text('Bu cihaz · ${me.id.substring(0, 8)} · port ${me.port}'),
                  const SizedBox(height: 4),
                  Text(
                    'Gelen dosyalar: $downloadDir',
                    style: Theme.of(context).textTheme.bodySmall,
                    overflow: TextOverflow.ellipsis,
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _PairedPeerCard extends StatefulWidget {
  const _PairedPeerCard({
    required this.peer,
    required this.onSendFiles,
    required this.onDropFiles,
    required this.onSendClipboard,
    required this.onForget,
  });
  final PeerDto peer;
  final VoidCallback onSendFiles;
  final ValueChanged<List<String>> onDropFiles;
  final VoidCallback onSendClipboard;
  final VoidCallback onForget;

  @override
  State<_PairedPeerCard> createState() => _PairedPeerCardState();
}

class _PairedPeerCardState extends State<_PairedPeerCard> {
  bool _dragging = false;

  @override
  Widget build(BuildContext context) {
    final p = widget.peer;
    final scheme = Theme.of(context).colorScheme;
    final card = Card.outlined(
      color: _dragging ? scheme.secondaryContainer : null,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: ListTile(
          leading: Badge(
            backgroundColor: p.online ? Colors.green : scheme.outline,
            smallSize: 10,
            child: Icon(platformIcon(p.platform), size: 32),
          ),
          title: Text(p.name),
          subtitle: Text(
            _dragging
                ? 'Göndermek için bırakın'
                : p.online
                ? 'Çevrimiçi${_isDesktop ? ' · dosyaları buraya sürükleyin' : ''}'
                : 'Çevrimdışı',
          ),
          trailing: Wrap(
            spacing: 4,
            children: [
              IconButton(
                tooltip: 'Dosya gönder',
                icon: const Icon(Icons.upload_file),
                onPressed: widget.onSendFiles,
              ),
              IconButton(
                tooltip: 'Panoyu gönder',
                icon: const Icon(Icons.content_paste_go),
                onPressed: widget.onSendClipboard,
              ),
              PopupMenuButton<String>(
                onSelected: (_) => widget.onForget(),
                itemBuilder: (_) => const [
                  PopupMenuItem(value: 'forget', child: Text('Cihazı unut')),
                ],
              ),
            ],
          ),
        ),
      ),
    );
    if (!_isDesktop) return card;
    return DropTarget(
      onDragEntered: (_) => setState(() => _dragging = true),
      onDragExited: (_) => setState(() => _dragging = false),
      onDragDone: (detail) {
        setState(() => _dragging = false);
        final paths = detail.files
            .map((f) => f.path)
            .where((s) => s.isNotEmpty)
            .toList();
        if (paths.isNotEmpty) widget.onDropFiles(paths);
      },
      child: card,
    );
  }
}

bool get _isDesktop =>
    Platform.isLinux || Platform.isWindows || Platform.isMacOS;

class _TransferTile extends StatelessWidget {
  const _TransferTile({required this.transfer, required this.peerName});
  final Transfer transfer;
  final String peerName;

  @override
  Widget build(BuildContext context) {
    final t = transfer;
    final scheme = Theme.of(context).colorScheme;
    final status = t.error != null
        ? 'Hata: ${t.error}'
        : t.completed
        ? 'Tamamlandı · ${humanSize(t.total)}'
        : '${humanSize(t.done)} / ${humanSize(t.total)}';
    return Card.filled(
      child: ListTile(
        leading: Icon(
          t.error != null
              ? Icons.error_outline
              : t.completed
              ? Icons.check_circle
              : (t.incoming ? Icons.download : Icons.upload),
          color: t.error != null
              ? scheme.error
              : (t.completed ? Colors.green : scheme.primary),
        ),
        title: Text(
          t.incoming ? '$peerName → bu cihaz' : 'Bu cihaz → $peerName',
        ),
        subtitle: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const SizedBox(height: 6),
            if (t.active) LinearProgressIndicator(value: t.fraction),
            const SizedBox(height: 4),
            Text(status, maxLines: 2, overflow: TextOverflow.ellipsis),
          ],
        ),
      ),
    );
  }
}

class _CodeView extends StatelessWidget {
  const _CodeView({required this.code, required this.hint});
  final String code;
  final String hint;

  @override
  Widget build(BuildContext context) {
    final style = Theme.of(context).textTheme.displaySmall?.copyWith(
      fontWeight: FontWeight.w600,
      letterSpacing: 6,
      fontFeatures: const [FontFeature.tabularFigures()],
    );
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text('${code.substring(0, 3)} ${code.substring(3)}', style: style),
        const SizedBox(height: 12),
        Text(hint, textAlign: TextAlign.center),
      ],
    );
  }
}

class _SectionTitle extends StatelessWidget {
  const _SectionTitle(this.text);
  final String text;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(4, 24, 4, 8),
    child: Text(text, style: Theme.of(context).textTheme.titleMedium),
  );
}

class _Empty extends StatelessWidget {
  const _Empty(this.text, {this.loading = false});
  final String text;
  final bool loading;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 8),
    child: Row(
      children: [
        if (loading) ...[
          const SizedBox(
            width: 16,
            height: 16,
            child: CircularProgressIndicator(strokeWidth: 2),
          ),
          const SizedBox(width: 12),
        ],
        Expanded(
          child: Text(text, style: Theme.of(context).textTheme.bodyMedium),
        ),
      ],
    ),
  );
}
