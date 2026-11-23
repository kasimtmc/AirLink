import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'rust/api/airlink.dart';

/// Something the user must answer: a pairing request or a file offer.
sealed class Prompt {
  const Prompt(this.requestId, this.peerName);
  final int requestId;
  final String peerName;
}

class PairingPrompt extends Prompt {
  const PairingPrompt(super.requestId, super.peerName, this.code);
  final String code;
}

class FileOfferPrompt extends Prompt {
  const FileOfferPrompt(
    super.requestId,
    super.peerName,
    this.files,
    this.totalSize,
  );
  final List<({String path, int size})> files;
  final int totalSize;
}

class Transfer {
  Transfer(this.id, this.peerId, this.incoming);
  final String id;
  final String peerId;
  final bool incoming;
  int done = 0;
  int total = 0;
  bool completed = false;
  String? error;
  List<String> files = const [];

  double get fraction => total == 0 ? (completed ? 1 : 0) : done / total;
  bool get active => !completed && error == null;
}

/// App state on top of the Rust node.
class AirLinkController extends ChangeNotifier {
  AirLinkNode? _node;
  DeviceInfoDto? me;
  List<PeerDto> peers = const [];
  final Map<String, Transfer> transfers = {};
  String? startError;

  final _prompts = StreamController<Prompt>.broadcast();
  final _notices = StreamController<String>.broadcast();
  Stream<Prompt> get prompts => _prompts.stream;
  Stream<String> get notices => _notices.stream;

  StreamSubscription<String>? _events;
  Timer? _refreshTimer;

  bool get ready => _node != null;
  List<PeerDto> get paired => peers.where((p) => p.paired).toList();
  List<PeerDto> get nearby =>
      peers.where((p) => !p.paired && p.online).toList();

  Future<void> start() async {
    try {
      final support = await getApplicationSupportDirectory();
      _node = await startNode(
        dataDir: support.path,
        downloadDir: p.join((await _downloadsBase()).path, 'AirLink'),
      );
      me = _node!.info();
      _events = _node!.events().listen(_onEvent);
      refresh();
      _refreshTimer = Timer.periodic(
        const Duration(seconds: 2),
        (_) => refresh(),
      );
    } catch (e) {
      startError = '$e';
    }
    notifyListeners();
  }

  Future<Directory> _downloadsBase() async {
    if (!Platform.isAndroid) {
      final d = await getDownloadsDirectory();
      if (d != null) return d;
    }
    return (await getExternalStorageDirectory()) ??
        await getApplicationDocumentsDirectory();
  }

  String get downloadDir => _node?.downloadDir() ?? '';

  void refresh() {
    final node = _node;
    if (node == null) return;
    final next = node.peers();
    if (!listEquals(next, peers)) {
      peers = next;
      notifyListeners();
    }
  }

  Future<String> startPairing(
    String peerId,
    Future<bool> Function(String code) confirmCode,
  ) async {
    final session = await _node!.startPairing(peerId: peerId);
    final ok = await confirmCode(session.code());
    final paired = await session.confirm(accept: ok);
    refresh();
    return paired ? 'Eşleşme tamamlandı' : 'Eşleşme reddedildi';
  }

  void respond(int requestId, bool accept) =>
      _node?.respond(requestId: requestId, accept: accept);

  Future<void> sendFiles(String peerId, List<String> paths) async {
    try {
      await _node!.sendFiles(peerId: peerId, paths: paths);
    } catch (e) {
      _notices.add('Gönderilemedi: ${_clean(e)}');
    }
  }

  Future<void> sendClipboard(String peerId) async {
    final data = await Clipboard.getData(Clipboard.kTextPlain);
    final text = data?.text ?? '';
    if (text.isEmpty) {
      _notices.add('Pano boş');
      return;
    }
    try {
      await _node!.sendClipboard(peerId: peerId, text: text);
      _notices.add('Pano gönderildi');
    } catch (e) {
      _notices.add('Pano gönderilemedi: ${_clean(e)}');
    }
  }

  void forget(String peerId) {
    _node?.forget(peerId: peerId);
    refresh();
  }

  void clearFinishedTransfers() {
    transfers.removeWhere((_, t) => !t.active);
    notifyListeners();
  }

  String peerName(String id) =>
      peers.where((p) => p.id == id).map((p) => p.name).firstOrNull ??
      id.substring(0, 8);

  void _onEvent(String raw) {
    final e = jsonDecode(raw) as Map<String, dynamic>;
    switch (e['event']) {
      case 'peer_discovered' || 'peer_lost' || 'pairing_completed':
        refresh();
        if (e['event'] == 'pairing_completed' && e['success'] == true) {
          _notices.add('${e['peer']['name']} ile eşleşildi');
        }
      case 'pairing_request':
        _prompts.add(
          PairingPrompt(e['request_id'], e['peer']['name'], e['code']),
        );
      case 'file_offer':
        final files = (e['files'] as List)
            .map((f) => (path: f['path'] as String, size: f['size'] as int))
            .toList();
        _prompts.add(
          FileOfferPrompt(
            e['request_id'],
            e['peer']['name'],
            files,
            e['total_size'],
          ),
        );
      case 'transfer_progress':
        final t = _transfer(e);
        t.done = e['bytes_done'];
        t.total = e['bytes_total'];
        notifyListeners();
      case 'transfer_completed':
        final t = _transfer(e)
          ..completed = true
          ..done = transfers[e['transfer_id']]?.total ?? 0
          ..files = (e['files'] as List).cast<String>();
        _notices.add(
          t.incoming
              ? '${t.files.length} dosya alındı'
              : '${t.files.length} dosya gönderildi',
        );
        notifyListeners();
      case 'transfer_failed':
        _transfer(e).error = e['error'];
        notifyListeners();
      case 'clipboard_received':
        Clipboard.setData(ClipboardData(text: e['text']));
        _notices.add('${e['peer']['name']} panosunu paylaştı');
    }
  }

  Transfer _transfer(Map<String, dynamic> e) => transfers.putIfAbsent(
    e['transfer_id'],
    () =>
        Transfer(e['transfer_id'], e['peer_id'], e['direction'] == 'incoming'),
  );

  String _clean(Object e) => '$e'.replaceFirst(
    RegExp(r'^AnyhowException\((.*)\)$', dotAll: true),
    r'$1',
  );

  @override
  void dispose() {
    _events?.cancel();
    _refreshTimer?.cancel();
    _node?.shutdown();
    super.dispose();
  }
}

String humanSize(int n) {
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  var v = n.toDouble();
  var i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return i == 0 ? '$n B' : '${v.toStringAsFixed(1)} ${units[i]}';
}
