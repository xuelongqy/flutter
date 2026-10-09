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
