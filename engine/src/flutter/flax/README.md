# Flax Engine Integration

Experimental macOS and Android arm64 integration based on Flutter 3.47.6 and Dart 3.13.5.
The macOS builds target macOS 15 or later; Android targets API 24 or later.
The exact Flutter, Dart, bridge and V8 SDK revisions are in `source.lock.json`.
The maintained branch is `flax/main`; version branches use `flax/<version>`.
iOS Hermes and other native V8 targets remain unimplemented.

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

Only one UI isolate per Dart isolate group is supported. Background runtime
creation and a second UI isolate in that group fail explicitly. GC does not
dispose application controllers or change Flutter's Element/State lifecycle.

`experiments/cross_heap_gc` is the preserved standalone experiment. It is not
the engine implementation or proof of real application acceptance. Real Flax
regressions and acceptance evidence belong to the Flax checkout.
