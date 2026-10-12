# Engine-owned Native Implementation

`runtime.cpp` implements the experimental function table using JSI: synchronous entry,
value IDs, object identity, conversion, host callbacks, exceptions, microtasks, and
teardown. `joint_gc.cc` attaches conditional edges between actual Dart and JS
objects using Dart weak handles and V8 CppHeap. One heap belongs to the Flutter
UI isolate; each runtime owns an independent Context and microtask queue.
`engine.h` declares the internal adapter factory and native Uint8Array view inspection.
`value_id.h` allocates opaque
engine-namespaced IDs without reuse or wraparound.

Public C ABI 2 is unchanged. `engine_gc.h` defines the separate internal GC
extension. `engine_binding.h` version 2 adds shared Uint8List/Uint8Array conversion.
`runtime.cpp` owns the common MutableBuffer leases: Dart storage is pinned through
`dart_flax_api.h`, and JS-origin storage stays live through Dart external-data finalizers.
Unpinning runs on the owning UI thread and VM shutdown retires remaining roots.
V8 retains BackingStore ownership; Hermes adopts ordinary ArrayBuffer storage and
preserves external storage during same-size transfer via its locked source patch.
Readonly, shared, resizable and detached storage is rejected. The maintained V8 SDK
must disable sandboxing; no implicit copy fallback is used for this binding contract.
macOS and Android select V8, while iOS selects Hermes.
The runtime implementation is copied from Flax revision recorded in
`../../source.lock.json`; JSI and V8 adapter licenses remain in their source
directories. Applications load these operations from FlutterMacOS, without
building or packaging another bridge library.
