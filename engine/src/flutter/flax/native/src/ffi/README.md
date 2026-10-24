# Direct C ABI support

Apple arm64 builds include this optional engine extension. Its checked table is
`FlaxEngineFfiApi` version 1; native ABI 2 is unchanged. JavaScript package installation
is controlled by Core, and ordinary target calls stay in shared JSI/libffi code.

`runtime.inc` owns loading, binding and retirement, `types.inc` owns ABI layouts,
`memory.inc` owns byte access, and `callbacks.inc` owns callback queues and finalizers.
The V8/Hermes fragments adapt byte storage and independent callback runtimes. The
runtime translation unit includes the common implementation once. Native test controls
are excluded from normal engine builds.

`ffi-script.inc` is generated from the matching `@flax/ffi` package for independent
callback runtimes. In the Flax repository, run `node tool/ffi_bundle.mjs` with the
package directory, a dependency directory containing its npm lockfile and installed
Buffer dependencies, and this output file. The dependency receipt and notices accompany
the generated source. Do not edit the embedded script by hand.

Libffi 3.4.8 sources and generated Apple arm64 headers are pinned under
`third_party/libffi`. Callbacks use its precompiled trampoline table. No generated RX
instructions or Dart calls are allowed on foreign native threads. Native producers
must be stopped and joined before callback entries are released.
