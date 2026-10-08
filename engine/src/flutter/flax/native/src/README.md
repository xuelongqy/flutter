# Engine-owned Native Implementation

`runtime.cpp` implements the experimental function table using JSI: synchronous entry,
value IDs, object identity, conversion, host callbacks, exceptions, microtasks, and
teardown. `joint_gc.cc` attaches conditional edges between actual Dart and JS
objects using Dart weak handles and V8 CppHeap. One heap belongs to the Flutter
UI isolate; each runtime owns an independent Context and microtask queue.
`engine.h` declares the internal adapter factory. `value_id.h` allocates opaque
engine-namespaced IDs without reuse or wraparound.

Public C ABI 2 is unchanged. `engine_gc.h` defines the separate internal GC
extension. macOS currently uses V8; other platforms are not implemented here.
The runtime implementation is copied from Flax revision recorded in
`../../source.lock.json`; JSI and V8 adapter licenses remain in their source
directories. Applications load these operations from FlutterMacOS, without
building or packaging another bridge library.
