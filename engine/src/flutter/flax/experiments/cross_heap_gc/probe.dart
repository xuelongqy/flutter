// Run with the source-built VM. All peers and callbacks are real heap objects.
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

typedef Tick = Double Function(Double);

void check(bool condition, String message) {
  if (!condition) throw StateError(message);
}

class Engine {
  static final lib = DynamicLibrary.open(Platform.environment['FLAX_GC_PROBE']!);
  static final init = lib
      .lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('flax_gc_init');
  static final create = lib
      .lookupFunction<
        Pointer<Void> Function(Pointer<NativeFunction<Tick>>),
        Pointer<Void> Function(Pointer<NativeFunction<Tick>>)
      >('flax_gc_create');
  static final make = lib
      .lookupFunction<
        Int32 Function(Pointer<Void>, Handle, Handle, Int32, Int32, Int32),
        int Function(Pointer<Void>, Object, Object, int, int, int)
      >('flax_gc_make');
  static final invoke = lib
      .lookupFunction<
        Double Function(Pointer<Void>, Int32, Int32),
        double Function(Pointer<Void>, int, int)
      >('flax_gc_call');
  static final error = lib
      .lookupFunction<
        Pointer<Uint8> Function(Pointer<Void>),
        Pointer<Uint8> Function(Pointer<Void>)
      >('flax_gc_error');
  static final collect = lib.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>(
    'flax_gc_collect',
  );
  static final alive = lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>(
        'flax_gc_alive',
      );
  static final stat = lib
      .lookupFunction<Int32 Function(Pointer<Void>, Int32), int Function(Pointer<Void>, int)>(
        'flax_gc_stat',
      );
  static final dropJs = lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
        'flax_gc_drop_js',
      );
  static final pressureJs = lib
      .lookupFunction<Void Function(Pointer<Void>, Int32), void Function(Pointer<Void>, int)>(
        'flax_gc_js_pressure',
      );
  static final gcJs = lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
        'flax_gc_js_collect',
      );
  static final destroy = lib
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
        'flax_gc_destroy',
      );
  final weak = <int, WeakReference<Counter>>{};
  final identities = <int, int>{};
  final roots = <Counter>[];
  late final NativeCallable<Tick> tick;
  late final Pointer<Void> pointer;
  bool closed = false, collectInCallback = false;

  Engine() {
    check(init(NativeApi.initializeApiDLData) == 0, 'Patched VM API unavailable');
    tick = NativeCallable<Tick>.isolateLocal((double rawId) {
      if (collectInCallback) collect(0, 1);
      final id = rawId.toInt();
      final counter = weak[id]?.target;
      check(counter != null, 'Dart target $id collected while called from JS');
      check(identityHashCode(counter) == identities[id], 'Dart identity changed');
      return (++counter!.value).toDouble();
    }, exceptionalReturn: -20000.0);
    pointer = create(tick.nativeFunction);
    check(pointer != nullptr, 'V8 creation failed');
    check(stat(pointer, 2) > 0, 'V8 native JIT code not observed');
  }

  @pragma('vm:never-inline')
  void setup(int count, String owner, int mode, {bool chain = false}) {
    Counter? previous;
    for (var id = 1; id <= count; id++) {
      final counter = Counter(this, id);
      weak[id] = WeakReference(counter);
      identities[id] = identityHashCode(counter);
      check(
        make(
              pointer,
              counter,
              counter.callback,
              id,
              (owner == 'js' || owner == 'both') && (!chain || id == 1) ? 1 : 0,
              mode,
            ) ==
            0,
        lastError(),
      );
      if ((owner == 'dart' || owner == 'both') && (!chain || id == 1)) roots.add(counter);
      if (chain && previous != null) previous.next = counter;
      previous = counter;
    }
  }

  String lastError() {
    final p = error(pointer);
    var length = 0;
    while (p[length] != 0) length++;
    return utf8.decode(p.asTypedList(length));
  }

  @pragma('vm:never-inline')
  double call(int id, {bool fromJs = false}) {
    if (closed) throw StateError('Engine closed');
    final value = invoke(pointer, id, fromJs ? 1 : 0);
    if (value.isNaN) throw StateError(lastError());
    return value;
  }

  @pragma('vm:never-inline')
  bool dartAlive(int id) => weak[id]?.target != null;

  @pragma('vm:never-inline')
  void verify(int count, bool expected, String label) {
    for (var id = 1; id <= count; id++) {
      check(dartAlive(id) == expected, '$label: Dart target $id');
      check(
        alive(pointer, id) == (expected ? 7 : 0),
        '$label: native peer $id mask ${alive(pointer, id)}',
      );
    }
  }

  Future<void> jointGc({int rounds = 3}) async {
    for (var n = 0; n < rounds; n++) {
      await Future<void>.delayed(Duration.zero);
      collect(0, 1); // Includes moving/compacting old generation collections.
    }
  }

  void close() {
    if (closed) return;
    closed = true;
    destroy(pointer);
    tick.close();
  }
}

class Counter {
  Counter(Engine engine, this.id) {
    callback = () => engine.call(id);
  }
  final int id;
  late final double Function() callback;
  Counter? next;
  int value = 7;
}

final rows = <Map<String, Object?>>[];
void record(Map<String, Object?> row) {
  rows.add(row);
  print(jsonEncode(row));
}

Future<void> ownership(String owner, int mode, {bool chain = false}) async {
  final e = Engine();
  final count = chain ? 12 : 1;
  final label = '$owner/mode-$mode/${chain ? 'chain' : 'cycle'}';
  try {
    e.setup(count, owner, mode, chain: chain);
    await Future<void>.delayed(Duration.zero);
    for (var n = 0; n < 4; n++) {
      Engine.collect(1, 0);
      e.verify(count, true, '$label minor-$n');
    }
    // Independent V8 collections must not discard a Dart-owned callback.
    Engine.gcJs(e.pointer);
    await e.jointGc();
    final expected = owner != 'neither' && !(owner == 'js' && mode == 3);
    e.verify(count, expected, label);
    if (expected) {
      if (owner == 'js') {
        check(e.call(1, fromJs: true) == 8, '$label JS invocation');
      } else {
        check(e.roots.single.callback() == 8, '$label Dart callback invocation');
      }
      for (var id = 2; id <= count; id++) check(e.call(id) == 8, '$label chain invocation');
    }
    e.roots.clear();
    Engine.dropJs(e.pointer);
    await e.jointGc();
    e.verify(count, false, '$label released');
    record({
      'case': label,
      'pass': true,
      'peers': count,
      'jointCollections': Engine.stat(e.pointer, 0),
      'crossHeapTraces': Engine.stat(e.pointer, 1),
      'v8NativeJitEvents': Engine.stat(e.pointer, 2),
    });
  } finally {
    e.close();
  }
}

Future<void> reentry() async {
  final e = Engine();
  try {
    e.setup(1, 'js', 1);
    await e.jointGc();
    e.collectInCallback = true;
    check(e.call(1, fromJs: true) == 8, 'JS -> Dart -> joint GC -> JS stack root');
    e.verify(1, true, 'reentry');
    e.collectInCallback = false;
    Engine.dropJs(e.pointer);
    await e.jointGc();
    e.verify(1, false, 'reentry released');
    record({'case': 'reentrant-gc', 'pass': true});
  } finally {
    e.close();
  }
}

Future<void> naturalPressure() async {
  final e = Engine();
  try {
    e.setup(1, 'neither', 0);
    await Future<void>.delayed(Duration.zero);
    final before = Engine.stat(e.pointer, 0);
    // Retain a bounded allocation window so this includes real old-generation
    // pressure. No explicit collect call in this phase.
    final window = <Uint8List>[];
    for (var i = 0; i < 3200 && Engine.alive(e.pointer, 1) != 0; i++) {
      window.add(Uint8List(1024 * 256)..[0] = i & 255);
      if (window.length > 160) window.removeAt(0);
      if (i % 40 == 0) await Future<void>.delayed(Duration.zero);
    }
    check(Engine.stat(e.pointer, 0) > before, 'No automatic major GC observed');
    e.verify(1, false, 'automatic allocation GC');
    record({
      'case': 'automatic-dart-allocation-gc',
      'pass': true,
      'jointCollections': Engine.stat(e.pointer, 0) - before,
    });
  } finally {
    e.close();
  }
}

Future<void> closeWithDartOwner() async {
  final e = Engine();
  try {
    e.setup(1, 'dart', 0);
    await e.jointGc();
    final counter = e.roots.single;
    final identity = identityHashCode(counter);
    e.close();
    Engine.collect(0, 1);
    check(
      identityHashCode(counter) == identity && counter.value == 7,
      'Closing JS changed the application-owned Dart object',
    );
    var rejected = false;
    try {
      counter.callback();
    } on StateError catch (error) {
      rejected = error.message == 'Engine closed';
    }
    check(rejected, 'Closed JS callback was not rejected');
    record({'case': 'close-with-dart-owner', 'pass': true});
  } finally {
    e.close();
  }
}

Future<void> benchmark(int peers, int ordinary) async {
  final e = Engine();
  try {
    e.setup(peers, 'dart', 0);
    Engine.pressureJs(e.pointer, ordinary);
    await e.jointGc();
    final v8Micros = <int>[];
    for (var i = 0; i < 12; i++) {
      await Future<void>.delayed(Duration.zero);
      final watch = Stopwatch()..start();
      Engine.gcJs(e.pointer);
      watch.stop();
      v8Micros.add(watch.elapsedMicroseconds);
      e.verify(peers, true, 'V8-only benchmark');
    }
    final micros = <int>[];
    for (var i = 0; i < 12; i++) {
      await Future<void>.delayed(Duration.zero);
      final watch = Stopwatch()..start();
      Engine.collect(0, 0);
      watch.stop();
      micros.add(watch.elapsedMicroseconds);
      e.verify(peers, true, 'benchmark');
    }
    micros.sort();
    v8Micros.sort();
    record({
      'case': 'joint-gc-cost',
      'pass': true,
      'bridgePeers': peers,
      'ordinaryJsObjects': ordinary,
      'samplesUs': micros,
      'medianUs': micros[micros.length ~/ 2],
      'maxUs': micros.last,
      'v8OnlySamplesUs': v8Micros,
      'v8OnlyMedianUs': v8Micros[v8Micros.length ~/ 2],
      'v8OnlyMaxUs': v8Micros.last,
    });
    e.roots.clear();
    await e.jointGc();
    e.verify(peers, false, 'benchmark released');
  } finally {
    e.close();
  }
}

Future<void> main(List<String> args) async {
  for (final mode in [0, 1, 2]) {
    for (final owner in ['dart', 'js', 'both', 'neither']) {
      await ownership(owner, mode);
    }
  }
  for (final owner in ['dart', 'js', 'both', 'neither']) {
    await ownership(owner, 1, chain: true);
  }
  await ownership('js', 3);
  await reentry();
  await naturalPressure();
  await closeWithDartOwner();
  if (args.contains('--bench')) {
    for (final ordinary in [0, 100000, 300000]) await benchmark(1000, ordinary);
  }
  record({
    'summary': 'pass',
    'cases': rows.length,
    'dartVersion': Platform.version,
    'os': Platform.operatingSystem,
    'script': Platform.script.toString(),
  });
  final path = Platform.environment['FLAX_GC_RESULTS'];
  if (path != null) File(path).writeAsStringSync(const JsonEncoder.withIndent('  ').convert(rows));
}
