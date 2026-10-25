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
extension. `engine_binding.h` extension 8 includes generic Type handles and shares primitive numeric typed lists,
ByteData and ByteBuffer through the same byte lease. BigInt uses exact decimal text
in the primitive ABI; native ABI 2 remains unchanged.
The copying entry acquires a Dart Uint8List address only after allocating the destination
ArrayBuffer, copies once and immediately releases it. It also accepts readonly sources.
No JS or allocating Dart operation runs while that address is acquired. Copied JS byte
ranges can be owned by external Dart data with `buffer_free` as its native finalizer;
the allocator does not depend on runtime lifetime.
`runtime.cpp` owns the common MutableBuffer leases: Dart storage is pinned through
`dart_flax_api.h`, and JS-origin storage stays live through Dart external-data finalizers.
Unpinning runs on the owning UI thread and VM shutdown retires remaining roots.
V8 retains BackingStore ownership across exported byte views and transfers.
Readonly, shared, resizable and detached storage is rejected. The maintained V8 SDK
must disable sandboxing; no implicit copy fallback is used for this binding contract.
macOS and Android select V8 JIT; iOS selects statically linked V8 jitless with uncaged
CppGC. The simulator and device use matching collector flags.
The runtime implementation is copied from Flax revision recorded in
`../../source.lock.json`; JSI and V8 adapter licenses remain in their source
directories. Applications load these operations from the maintained Flutter framework,
without building or packaging another bridge library.
