# Public Native Headers

[runtime.h](runtime.h) is the canonical experimental C ABI. It declares opaque runtime
handles, runtime-owned value IDs, value/error buffers, callbacks, and a versioned
function table. JSI and C++ types stay behind this boundary.

Callers zero-initialize error records, release returned buffers with the supplied
allocator functions, and obey owned/borrowed handle rules. Runtime pointers must not be
used after destruction. The Dart wrapper provides disposal and reference guards.

Regenerate Dart declarations in the Flax workspace with `dart run melos run ffi:generate`
and verify them with `ffi:check`. Incompatible changes require an ABI version update.
`engine_gc.h` and `engine_binding.h` are separately versioned engine extensions.
Binding extension 1 registers shared member layouts and invokes current JS receivers
with flat typed arguments. Keep these headers identical to the Flax workspace copies.
See
[engine build and ownership](../../../README.md).

`engine_workers.h` is an optional, separately versioned Dedicated Worker extension.
It creates a detached V8 runtime with explicit references and no Dart object peers.
The owning Dart isolate enters and disposes it; only channel interruption and
lifecycle counts are thread safe. Native message queues retain serialized values and
transferred BackingStores, while Dart native ports carry wakeups only.

`engine_images.h` declares the optional shared image codec/bitmap operations. The same
header is mirrored in Flax and generated into its Dart FFI bindings. CPU codecs require
no Canvas 2D plugin; ui.Image/Picture operations remain on the owning UI isolate.

`engine_webgpu.h` declares optional macOS interop profile 1 for mapped-buffer lifetime
and Metal producer/consumer fences. It does not change the main ABI or Worker profile.
`dawn_interop.h` is the internal Objective-C selector contract, compiled only under the
maintained macOS interop define. Dawn itself remains a separately packaged native asset.
