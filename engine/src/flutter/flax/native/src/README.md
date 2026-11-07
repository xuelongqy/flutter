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

Dedicated Workers use `engine_workers.h` and `v8-jsi/src/jsi_abi/workers.inc`.
Their V8 Isolates have no joint Dart heap; explicit handles and host callbacks belong
to the creating background Dart isolate. V8 Lockers allow that isolate to migrate
between scheduler threads. Foreground tasks wake its native port and microtasks run
at host checkpoints. Dart-owned shared-buffer leases and object peers are rejected.
Native channel interruption precedes runtime disposal and never calls Dart from a
foreign thread. Ordinary runtime creation retains its Flutter UI isolate check.

Ordinary V8 external-memory collection requests coalesce their Dart follow-ups for
8 ms on the owner runner. This avoids a full Dart pass for every transferred buffer.
V8 accounting/collection, ordinary JS allocation pressure, forced/last-resort
collections and direct Dart requests keep their existing paths. An actual Dart trace
cancels pending external work, and the delayed task holds only a weak heap reference.
The fixed point and explicit disposal are unchanged; peak memory can increase and
individual full-collection pauses remain.

Detached Worker Promise rejection callbacks queue native Promise/reason references and
release them when the owning host checkpoint drains the records. JS event handlers run
after the microtask checkpoint. Error locations use captured exception frames rather
than a new Message at the reporting helper.

## One-way joint GC experiment

This isolated branch packages the Pixel 4 Android arm64 Release/AOT prototype.
The local trial record is `flax-joint-gc-one-way-20261108-1/TRIAL.md`.
`../../tools/apply_dart_patch.py` reconstructs the tested Dart VM from the SDK
revision in `../../patches/dart_revision`. The consolidated `dart.patch` applies
to that upstream revision. `dart-joint-gc.patch` upgrades the same revision
with the maintained Flax patches already applied.

The one-way policy is opt-in. On the owning UI isolate, call
`flax_experiment_incremental_joint(1)` once after creating the Flax runtime
and before starting the workload. A return value of `1` confirms selection.
Mode `0` selects the staged control in the same engine.

Ordinary Dart collections guard bridge peers and post a coalesced owner request.
An eligible V8 major runs fresh staged Dart marking. After one second the owner
attempts a deadline major. The r3 gate admits the UI isolate with at most one
eligible Dart Worker in the same group. Failed admission preserves the peers
and retries. Reclamation can be postponed while multiple same-group Workers
remain active.
