# Flax Engine Integration

Experimental macOS, Android and iOS arm64 integration based on Flutter 3.47.6
and Dart 3.13.5. macOS targets 15 or later; Android targets API 24 or later. The iOS
producer targets 16.3 or later and supports simulator debug and device release/AOT.
The exact Flutter, Dart, bridge, V8 SDK and Hermes source revisions are in `source.lock.json`.
The maintained branch is `flax/main`; version branches use `flax/<version>`.
Other native V8 targets, device debug/profile and simulator profile/release remain unimplemented.

## Build

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
python3 engine/src/flutter/flax/tools/test_build_hermes.py
```

Use the official local-engine flow from an application checkout:

```sh
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_mac_debug_arm64 --local-engine-host=flax_mac_debug_arm64 test
```

For profile or release, build the matching mode and select `flax_mac_profile_arm64`
or `flax_mac_release_arm64` for both engine flags. The macOS SDK uses system libc++;
`flax_system_cxx` makes the engine use matching headers and ABI. The build
GN selects Xcode's linker for its SDK's TAPI format. Tool pins remain intact.
Framework dependencies are copied into `FlutterMacOS.framework/Versions/A/Libraries`;
the test executable uses an adjacent `Libraries` directory.

For Android, put Python 3.11's `python3` on PATH and install the NDK revision recorded
by the verified V8 SDK's
`metadata.hostTools.androidNdk` and set `ANDROID_HOME`. The engine uses that NDK's
C++ headers and the SDK's immutable `libc++_shared.so`; Flutter's pinned compiler,
C sysroot and compiler runtime remain unchanged. The SDK libraries are packaged
in the existing Flutter engine JAR, so applications carry one V8 library set.

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

For the iOS arm64 simulator, the helper downloads the pinned Hermes source archive,
checks every patch, and builds host compiler tools plus static interpreter libraries.
Existing release SDK assets are not rebuilt or changed. CMake 3.24 or newer, Ninja and
Xcode are required. Source stamps reject edits, missing patches and recipe changes;
use `build_hermes.py --cache /fresh/path` and pass its output with `--hermes-build` to
keep a previous producer intact.

```sh
python3 engine/src/flutter/flax/tools/build_engine.py --target ios-simulator-arm64 --mode debug
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_ios_debug_sim_arm64 --local-engine-host=flax_mac_debug_arm64 build ios --simulator --debug
```

Build the matching macOS debug host tools first and set the application deployment
target to 16.3 or later. Flutter links Hermes, JSI and Boost.Context statically into
`Flutter.framework`; applications carry no standalone Hermes library or JIT entitlement.
The Flax gate uses `FLAX_CHECK_TARGET=ios-simulator-arm64 FLAX_CHECK_DEVICE=<simulator-id>
dart run tool/check_engine_application.dart debug`. It checks interpreter identity,
joint GC, the simulator framework, signatures and relocation.

For a physical arm64 iPhone, build the matching macOS release host tools, then the
device release engine. The producer keeps device and simulator static libraries and
manifests separate and rejects a mismatched input target:

```sh
python3 engine/src/flutter/flax/tools/build_engine.py --target ios-device-arm64 --mode release
/path/to/flutter/bin/flutter --local-engine-src-path=/path/to/flutter/engine/src --local-engine=flax_ios_release_arm64 --local-engine-host=flax_mac_release_arm64 build ios --release
```

In the Flax checkout, use `FLAX_CHECK_TARGET=ios-device-arm64`,
`FLAX_CHECK_DEVICE=<iphone-id>` and `FLAX_IOS_TEAM=<team-id>`, then run
`dart run tool/check_engine_application.dart release`. The device gate requires real
development signing and provisioning, verifies Dart AOT and the embedded Hermes closure,
and installs the relocated application after source removal. iOS requires no JS JIT
entitlement. These gates do not establish release distribution or Hermes GC/frame performance.

Internal binding extension 8 includes the VM fix for recursive function bounds under
an enclosing generic declaration. The VM only resolves type-parameter bounds for its
`Never` normalization; expanding a nominal F-bound such as `Comparable<T>` would
recurse indefinitely. Flax rejects older engines before loading these bindings.
The public native ABI remains 2. This update requires rebuilding the matching engine;
it does not rebuild the V8 SDK or change its lock.

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
Closing a Context releases only its edges; isolate shutdown cleans remaining
runtimes before Dart handles become invalid.

Hermes has no V8-style Context API. Each session therefore owns a Hermes VM, while one
UI-isolate coordinator follows conditional references across all of them to a fixed
point. Hermes's existing Hades marker provides reachability and weak processing; no
separate JS heap scanner is added. Ordinary Hades marking remains concurrent. During
joint tracing all participating VM markers reach the final marking phase before weak
processing, so mixed callback chains across sessions preserve their Dart business roots.
Active JS jobs defer joint tracing and conservatively retain bridge peers until a safe
owner-thread checkpoint. Closing a session clears only its VM's edges.

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
operations, independent of V8/Hermes. Typed construction validates the real field Types;
a null target creates a runtime layout from sorted public field names. No per-layout
Dart constructor or business Record peer is needed. The maintained Dart patch also marks
referenced Record shapes as externally allocatable and synthetic fields as externally
populated during AOT type-flow analysis. Compile the frontend from this source when
building local engine tools; an old frontend may prune a native-only field getter.
