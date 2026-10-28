# Flax Engine Integration

Experimental macOS, Android and iOS arm64 integration based on Flutter 3.47.6
and Dart 3.13.5. macOS targets 15 or later; Android targets API 24 or later. The iOS
producer targets 16.3 or later and supports simulator debug and device release/AOT.
The exact Flutter, Dart, bridge, V8 SDK revisions are in `source.lock.json`.
The maintained branch is `flax/main`; version branches use `flax/<version>`.
Other native V8 targets, device debug/profile and simulator profile/release remain unimplemented.

## Build

The companion Canvas plugin uses the Canvas engine extension:
`canvas_decoder.inc` validates protocol v1, owns drawing state and records all 48
commands and 13 path operations into Flutter DisplayLists. One plugin installation
owns its surface states and image registrations on the UI isolate. Its non-leaf
entry accepts ordinary and external Uint8List storage synchronously; failed batches
publish neither state nor a picture. The canonical `canvas.h` header is maintained
with the Flax Canvas package. Dart retains widgets, history composition and async I/O.
The plugin requires this engine extension and has no Dart decoder fallback. Installation
uses the dispatch entry point without an exact feature-version gate. Preserve existing
operation IDs and payload semantics when adding capabilities. The legacy version query
remains for prior callers and diagnostics; compatible additions do not bump it. The plugin
retains stroke hit tests, synchronous renderer pixel readback, text options, maxWidth
and metrics, and adds ordered numeric filters, source-alpha shadows, full-surface
composition and font-relative spacing. Text uses primary-face metrics and BASE tables. ImageData uploads premultiply straight RGBA; reset clears the output.
Readback waits at most 10 seconds and never pins Dart bytes while waiting.
Canvas shares the Session's existing FlaxFontCollection with drawing and
measurement, refreshes public font registrations and enables native bold/italic
synthesis. A host-independent CPU encoder exports straight RGBA8 as JPEG or WebP
on a Dart worker isolate; ordinary drawing/readback operations remain on the UI
isolate. PNG keeps the existing asynchronous Flutter encoder.
roundRect preserves dash origin, winding and the final (x,y) subpath. SVG Path2D
retains complete segments before a syntax error, delegates geometry to Skia and
starts the required final subpath; addPath does the same after its transform.
Text accepts the companion parser's normalized fractional weights and oblique
angles, passes wght/slnt variation coordinates and shears static oblique faces
around their baseline while preserving gradient/pattern coordinates.

The Canvas decoder is part of the maintained engine builds below. Only the production
Canvas dispatch is exported; the early direct-JSI fillRect probe has been removed.
The companion Flax checkout retains decoder and drawImage benchmarks in
`experiments/canvas_native`. Native ABI 2 and the binding table are unchanged.

From the Flutter checkout, after the ordinary engine dependencies are synced:

```sh
python3 engine/src/flutter/flax/tools/build_engine.py --sdk /path/to/verified/v8-macos-arm64 --mode debug
```

The helper verifies the SDK manifest and library hashes and applies the Dart
patch. DEPS also applies this patch after syncing the exact Dart revision.
Repeated application is safe; a wrong revision or conflict fails without
resetting local edits. Verify those boundaries with:

```sh
python3 engine/src/flutter/flax/tools/test_apply_dart_patch.py
python3 engine/src/flutter/flax/tools/test_sdk_info.py
```

Use the official local-engine flow from an application checkout:

```sh
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_mac_debug_arm64 --local-engine-host=flax_mac_debug_arm64 test
```

For profile or release, build the matching mode and select `flax_mac_profile_arm64`
or `flax_mac_release_arm64` for both engine flags. The macOS build also produces the
matching const-finder snapshot. Its managed `flax:runtime-icon-data` factory exception
keeps business-owned icon fonts separate from host constant-font subsetting; ordinary
unmarked dynamic IconData is still rejected. Rebuild this helper and refresh the Flutter
tools snapshot after changing their sources. The macOS SDK uses system libc++;
`flax_system_cxx` makes the engine use matching headers and ABI. The build
GN selects Xcode's linker for its SDK's TAPI format. Tool pins remain intact.
Framework dependencies are copied into `FlutterMacOS.framework/Versions/A/Libraries`;
the test executable uses an adjacent `Libraries` directory.

The Canvas mobile experiment builds complete isolated targets, including renderer
shaders and framework/JAR packaging. Android exports the Canvas dispatch symbols. The companion `experiments/canvas_native/run_mobile.dart` runs the shared
Canvas command, UI, bitmap/font and original-threshold WPT cases in an installed app.

For Android, put Python 3.11's `python3` on PATH and install the NDK revision recorded
by the verified V8 SDK's
`metadata.hostTools.androidNdk` and set `ANDROID_HOME`. The engine uses that NDK's
C++ headers and the SDK's immutable `libc++_shared.so`; Flutter's pinned compiler,
C sysroot and compiler runtime remain unchanged. The SDK libraries are packaged
in the existing Flutter engine JAR, so applications carry one V8 library set.
The build helper also refreshes `flutter.jar`, the native architecture JAR and
`artifacts.zip`; ordinary APK builds therefore receive the newly built engine.

```sh
ANDROID_HOME=/path/to/android-sdk python3 engine/src/flutter/flax/tools/build_engine.py --target android-arm64 --sdk /path/to/verified/v8-android-arm64 --mode debug
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_android_debug_arm64 --local-engine-host=flax_mac_debug_arm64 build apk --debug --target-platform=android-arm64
```

Android uses the matching macOS host build for Dart tools. For release, use
`flax_android_release_arm64` and `flax_mac_release_arm64`. In the Flax checkout,
`FLAX_CHECK_TARGET=android-arm64 FLAX_CHECK_DEVICE=<adb-id> dart run
tool/check_engine_application.dart debug|release` builds and launches the real
acceptance APK, verifies V8 JIT, and audits library hashes, signatures and 16 KB
page compatibility. Release uses Dart AOT. The APK runs from a relocated path
after the temporary application source is removed.

iOS uses V8 jitless with uncaged CppGC. Build the matching macOS host tools first,
then select the verified static SDK for the exact device or simulator target:

```sh
python3 engine/src/flutter/flax/tools/build_engine.py --target ios-simulator-arm64 --mode debug --sdk /path/to/verified/v8-ios-simulator-arm64
python3 engine/src/flutter/flax/tools/build_engine.py --target ios-device-arm64 --mode release --sdk /path/to/verified/v8-ios-device-arm64
```

The iOS SDK producer is pinned by each target's `sdkProducerRevision` in
`source.lock.json` (the local candidate; no SDK publication is implied).
The SDK lock verifies archive, header, recipe and target hashes. Its private Abseil and
Unicode namespaces are isolated from Flutter's copies. The 0.3.2 iOS candidates disable
JIT and TurboFan while enabling Wasm through
DrumBrake with explicit bounds checks and V8 pointer compression. CppGC remains uncaged,
with its own pointer compression and young generation disabled. These engine flags are
independent of the reserved host sandbox policy. An optional module extension version 1
adds original JavaScript ESM, JSON synthetic modules and Wasm source/evaluation imports,
without changing ABI 2 or UI protocol 24; the host owns asynchronous source loading.
V8 retains its default foreground queues. Worker tasks notify the owning Dart native
port; only the owning thread pumps tasks and drains microtasks. Notifications are
unregistered before Context destruction, including late asynchronous Wasm completion.

V8, JSI and CppGC link statically into `Flutter.framework`; no standalone JS engine
library or archive is packaged in the app. `FlaxV8Notices` preserves the exact SDK
manifest and upstream notices. iOS requires no JS JIT entitlement. Consumers target iOS
16.3 or later and use Flutter's ordinary local-engine flags:

```sh
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_ios_debug_sim_arm64 --local-engine-host=flax_mac_debug_arm64 build ios --simulator --debug
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_ios_release_arm64 --local-engine-host=flax_mac_release_arm64 build ios --release
```

With the Wasm-enabled 0.3.2 SDK, physical app targets also need Xcode's **Extended
Virtual Addressing** capability and a provisioning profile containing
`com.apple.developer.kernel.extended-virtual-addressing = true`. V8 pointer compression
reserves a 4 GiB virtual region, rather than committing 4 GiB of RAM. Missing address
space aborts V8 initialization before any module executes. The previous SDK without
Wasm did not require this capability. The Flax example tools configure it and the
device application audit checks the signed entitlement before installation.

From the Flax checkout, set `FLAX_CHECK_TARGET`, `FLAX_CHECK_DEVICE` and, for a physical
iPhone, `FLAX_IOS_TEAM`. Run `tool/check_engine_application.dart debug` for the simulator
or `release` for the device. The gate verifies actual jitless execution, runtime/UI/GC,
production optional FFI, engine input hashes, signatures and relocation. The device gate
also verifies development provisioning and Dart AOT. Use `FLAX_CHECK_STRESS_SECONDS=900`
for the separate sustained device GC gate. These checks do not certify distribution
signing. Simulator profile/release and device debug/profile remain unsupported.

Internal binding extension 8 includes the VM fix for recursive function bounds under
an enclosing generic declaration. The VM only resolves type-parameter bounds for its
`Never` normalization; expanding a nominal F-bound such as `Comparable<T>` would
recurse indefinitely. Flax rejects older engines before loading these bindings.
The public native ABI remains 2. This update requires rebuilding the matching engine;
it does not rebuild the V8 SDK or change its lock.

## Generated default calls

The maintained Dart compiler lowers `flaxInvokeWithDefaults` markers from generated
bindings to fixed entries. Slots and 32-bit presence words become scalar arguments;
the marker lists are removed. The entry uses native defaults, including private values,
and preserves generic scopes, virtual overrides and lexical `super`. Dynamic callback
values retain Flax's ordinary shared invocation path.

The transformation is part of the revision-checked Dart patch and runs in the ordinary
platform compiler and frontend server. SDK virtual entries are generated before the
JIT core snapshot. Rebuild the matching host and target modes with `build_engine.py`;
an old precompiled SDK is rejected instead of silently missing entries. Applications
need no compiler override. Verify the built compiler and SDK with:

```sh
python3 engine/src/flutter/flax/tools/check_default_calls.py --mode debug
python3 engine/src/flutter/flax/tools/check_default_calls.py --mode release
```

Flutter's application build already tracks compiler and SDK artifact contents. Local
engine resident/test Kernel cache keys also include those contents, so rebuilding in
the same output directory cannot select a stale initial Kernel cache. Restart the tool
and application after rebuilding the compiler or SDK. Compatible source hot reload
keeps fixed-entry identities stable and preserves existing Dart/JS instances; SDK and
unsupported class-shape changes still require a restart. This compiler extension does
not change the native ABI, UI protocol or joint-GC ownership.

Incremental compilation also invalidates serialized dependency groups when fixed
helpers are added to a previously cached library. Their owning libraries are emitted
in the reload delta. The revision-checked patch includes this repair for cold mixed
SDK/application caches; `dart-startup.patch` upgrades earlier patched checkouts
without resetting unrelated Dart edits.

## JavaScript startup and execution

The optional `engine_execution.h` table (version 1) combines native results with their
value descriptions and reads borrowed callback arguments in one batch. Its canonical
header is mirrored from Flax; no application-specific symbols or helper libraries are
required. Native ABI 2 and binding extension 8 are unchanged.

Normal owning FlaxViews prepare module factories and compile the business entry on a
worker, then install Dart host callbacks and activate that same V8 context on UI.
Preparation is currently enabled only on macOS for a sole pristine runtime with no
Dart peers, external handles or FFI installation. Existing sessions, configured
runtimes and other platforms use staged UI startup. A module factory requiring Dart
falls back in the same runtime after clearing its pristine preparation context.
Business evaluation and synchronous Builder/LayoutBuilder callbacks remain on UI.

A new synchronous runtime interrupts an unfinished pristine worker and releases the
heap before creation. Polling then discards that incomplete context and selects UI
initialization; completed preparation can still be adopted. Runtime-specific guards
prevent access to partial globals until polling finishes.

Preparation holds the shared V8 heap exclusively. Dart GC skips that lock while the
bridge has no Dart peers; cancellation, disposal and hot restart terminate and join
the worker before retiring the heap. A worker cannot call Dart host callbacks. These
rules preserve joint-GC ownership and avoid blocking another live Session on a worker.
Build the ordinary Debug/Release targets above to include the extension and compiler
repair; the verified V8 SDK and its locks remain unchanged.

## Ownership

Dart's ordinary parallel marking remains enabled. Conditional tracing then
roots JS peers reachable from Dart and marks Dart targets reachable through
V8 CppHeap, to a fixed point. Direct targets of Dart-rooted JS peers are marked
first. If every Dart target is already marked and no JS collection was requested,
the Dart pass skips JS heap traversal. Unmarked targets always require full joint
tracing. Bridge indices contain weak business values. JS work requests an idle V8
collection; Flutter
idle notifications check Dart ownership without marking the JS heap changed.
Requests are batched on the UI thread. GC callbacks never invoke business code.
Only a completed V8 major collection requests another joint pass. Minor, incremental
and weak-callback phases do not escalate to full Dart/V8 collections; idle work still
provides eventual collection without creating a frame-by-frame GC feedback loop.
Closing a Context releases only its edges; isolate shutdown cleans remaining
runtimes before Dart handles become invalid. A Dart origin may expire while its JS
peer still retains another Dart target. Conditional marking ignores finalized, still
registered weak handles; it never resurrects them or accepts unregistered handles.

The proactive one-second timer defers collection while the native frame clock is less
than 100 ms old. Deferral preserves pending JS changes. Dart/V8 allocation pressure
still requests collection, and stopping animation allows eventual idle cleanup. Gaps of
100 ms or more may admit proactive collection; this is an inactivity heuristic.

Only one UI isolate per Dart isolate group is supported. Background runtime
creation and a second UI isolate in that group fail explicitly. GC does not
dispose application controllers or change Flutter's Element/State lifecycle.

Proxy getters return values through Flax's typed Dart converter, including Promise
values for Future/FutureOr properties and lazy Stream references. Native dispatch
still rejects a Promise returned by the setter function; assigning a Future-valued
property does not make the Dart assignment asynchronous. Native ABI 2 is unchanged.

`experiments/cross_heap_gc` is the preserved standalone experiment. It is not
the engine implementation or proof of real application acceptance. Real Flax
regressions and acceptance evidence belong to the Flax checkout.


Record transfer uses common VM `Dart_FlaxRecordFields` and `Dart_FlaxNewRecord`
operations, independent of the engine adapter. Typed construction validates the real field Types;
a null target creates a runtime layout from sorted public field names. No per-layout
Dart constructor or business Record peer is needed. The maintained Dart patch also marks
referenced Record shapes as externally allocatable and synthetic fields as externally
populated during AOT type-flow analysis. Compile the frontend from this source when
building local engine tools; an old frontend may prune a native-only field getter.

## Session-private fonts

The host-only dart:ui extension `FlaxFontCollection` owns a private dynamic provider
and SkParagraph collection. `addFont(bytes, family: name)` registers a logical name in
that domain. Pass it to `ParagraphBuilder(..., fontCollection: domain)`; ordinary host
ParagraphBuilder and FontLoader keep their public behavior. Public asset, test and
system managers are reused, and later host registrations refresh private lookup caches.

The root FontCollection keeps weak content-cache entries. A checksum bucket plus an
exact byte comparison shares parsed typefaces for equal private font files. Family
registrations remain private. Disposing a Dart collection releases its owner; live
Paragraphs/providers retain the source until their final retirement. The embedding
runtime must retain a domain for native subtrees that outlive their JS Session.

Final source retirement marks Impeller glyph atlases stale. A raster boundary replaces
atlases and purges unused strike/typeface caches without invalidating in-flight draws.
Closing one owner of still-shared source data does not retire that source. Other live
fonts can require glyph cache warmup after retirement of a different source. This is
not an immediate resident-memory purge. `flaxScopedFontStats` reports private
collections, aliases, unique source objects and source bytes.

The superseded global managed-font registration API is removed. Host family names have
no reserved Flax prefix. The build helper installs generated sky_engine declarations
for analyzer/Pub. Rebuild matching Debug/Release engines whenever the dart:ui interface
changes.

Canvas bitmap decoding and explicit pixel conversion also run through CPU-only
extension operations 16/17 in a Dart worker isolate. SkCodec handles embedded profiles
and EXIF; crop/resize/flip preserves straight pixels until requested alpha conversion.
No UI handles cross isolates. Command 48 draws the existing ImageData extension using
the regular image paint/state path. Existing operation IDs and signatures are unchanged.

Canvas snapshots and readback explicitly select RGBA8 instead of the display
format. Explicit MSAA snapshots use lossless resolve textures; other Flutter snapshots
retain their format selection. Snapshot images use Flutter deferred-image ownership.

Metal Gaussian blurs encode dependent passes in one command buffer. For an
integer-aligned, unity-scale decal blur with matching RGBA8, BGRA8 or XR10 textures,
the vertical pass samples the existing source and transparent gutter directly,
avoiding an identity copy. Both directional kernels and the source bitmap layers
remain unchanged. Bounded, rescaled, mipmapped and other-format inputs retain the
copy pass. Other backends retain separate buffers for the older Adreno workaround.
Metal also continues a simple child layer restore into an unused offscreen parent
within the same command buffer. Rectangular scissor-only clips defer pass creation;
the first draw installs the saved scissor, and clipped fills cannot become a full-target
clear. Filters and advanced blends submit their input dependencies first. Source
quantization and per-operation blending remain intact.
Blur/color-matrix layer filters additionally prepare their dependencies before opening
an unused output encoder and share that output's command buffer on Metal. The source
is submitted early; other filters and backends retain their scheduling. The render
passes and screen sampling remain separate, preserving all quantization boundaries.
An unused transparent Metal layer can retain an already resolved source texture in
place of an integer-aligned copy. Different extents require an unclipped source and a
translation-only, unity-scale decal-blur/matrix chain that preserves transparent black.
The logical layer bounds remain unchanged; the snapshot carries the source's integer
offset. Later drawing materializes the original copy before proceeding. Downsampled,
other-effect, clipped and previously painted targets retain their existing passes.
Gradient dithering uses one RGBA8 code value (1/255), so its sub-half-step noise
preserves exactly representable endpoints. The previous 1/64 amplitude could turn
opaque primary-color plateaus into 254 during Metal readback.
