# Top-level unlinkJoint experiment

This opt-in fixture preserves the Android Release/AOT prototype on the
batched one-way GC baseline `88ee8962d2d7963d8eba137748db99b1ea02629b`.
Use a disposable engine checkout and benchmark app. The baseline engine and
Dart SDK patches, including Dart's second root scan, are the inputs. The
native patch touches only `joint_gc.cc` and `joint_gc.h`. These files under
`experiments/` have no build integration.

The assembled `@flax/core` module exports `unlinkJoint(ref)`. A first successful
unlink returns `true`. A repeat returns `false`. Wrong and foreign capabilities
throw `TypeError`. Object unlink closes the view and all references to that
view while preserving the independent target alias and the reverse facade.
Listener unlink removes one generated `TextEditingController` registration.
A self-unlinking callback finishes its current call, and another registration
of the same function still dispatches.

Use trusted fixture code only. The app exposes host functions for setup,
binding and reads during bootstrap. Native setup requires the batched
one-way mode and binds the control function to the owner runtime context
and session. Public capability acquisition, untrusted bootstrap and
async callback contracts are outside this experiment.

## Frozen inputs

- **B** is the evidence directory `flax-joint-gc-local-commit-20261108-1`.
  Use `package-batch/trial.apk` for asset assembly and `app-build/app` for the
  benchmark app. The commands below pin their input bytes.
- **P** is `flax-unlink-joint-20261108-1`. The archived `RESULTS.sha256`
  has SHA-256 `e170b1f0aa682edac364bd3f1a7996c6bf8122168a329b1a07e334aea7283bbf`.
  It covers the retained source, build receipts and trial results. In
  particular, `NATIVE_SOURCE.sha256` pins both versions of the native files,
  and `SOURCE_READY.json` under `source/` pins the template assets.
- The original package container is **C8**, the `control8.apk` under
  `flax-staged-gc-20261106/build-preparation/v8/ui-transport-pair/package/`.
  Its SHA-256 is `c72bf093c7b68bb1ccbba904f64bdb9e8de717e9bb2dfa3fcd7e1275033c5eda`.
  Its core, module manifest and workload inputs are byte-identical to B's.

## Reconstruct the source and assets

Set `B` to the absolute path to the B evidence directory, `ENGINE` to the
absolute path to a clean disposable Flutter checkout at the baseline, and
`SCRATCH` to an existing writable directory on the workspace volume. Run
from the Flutter checkout containing this README. The app and asset output
directories must be new. Assembly uses Python 3.11 or newer. The JS test
uses Node with VM modules and `unzip`.

```sh
set -e
experiment="$PWD/engine/src/flutter/flax/experiments/unlink_joint"
template="$B/package-batch/trial.apk"
app="$SCRATCH/app"
generated="$SCRATCH/assets-generated"
mkdir -p "$SCRATCH/tmp"
export TMPDIR="$SCRATCH/tmp" TMP="$SCRATCH/tmp" TEMP="$SCRATCH/tmp"

test "$(git -C "$ENGINE" rev-parse HEAD)" = 88ee8962d2d7963d8eba137748db99b1ea02629b
git -C "$ENGINE" apply --check "$experiment/native.patch"
git -C "$ENGINE" apply "$experiment/native.patch"

mkdir "$app"
cp -R "$B/app-build/app/lib" "$app/lib"
cp "$B/app-build/app/pubspec.yaml" "$B/app-build/app/pubspec.lock" "$app/"
(
  cd "$app"
  shasum -a 256 -c <<'SUMS'
e32e56961aa612194b3f9834ced967c7d6a31fdf0962037a306e5539787f296d  lib/main.dart
c995af63dfc0eca1212685ab51375cad81870c8fe06f32051f023de45650aab7  lib/paced.dart
7a38fa1048ddb3f43df061df8670a14699bf9aa084c6f44af60cffcb9d3e9779  lib/trial.dart
2e094468f92db69ad38aedb9a6409fdb50e3535730070e259ce80511066519ab  pubspec.yaml
d952a9338919daf83c40ceff2844d796ae50f06cb2d2bef4f4c7d0b240f83861  pubspec.lock
SUMS
  git apply --check "$experiment/app.patch"
  git apply "$experiment/app.patch"
)

# The assembler checks the entire B APK before extracting its inputs.
python3 "$experiment/assemble_assets.py" "$template" "$generated"
node --experimental-vm-modules "$experiment/verify-js.mjs" "$template" "$generated"
(
  cd "$generated"
  shasum -a 256 -c <<'SUMS'
43741636b04553db4f8f7b63d945e8f77b5e5273bc6db95bc36b5db8f267464e  core.js
dad8749825aba966f34795695db2719f6ea3f1d40403b7a5bf699d886f1ad6e6  base.js
06c1f59bbbb289c6baa73693f24948e52f9333cfdde6f76a80d8d16d23bdfdcd  manual.js
cd5e8ff4956869d9c1330c1158573c857b3bac68ef61fd2616bdddfc28f6b0d9  assets/modules/_flax_core-14fe61593557.js
bedf6ddf8bac34e4cfebc1df90a08a6426d844c258296dc85f6c571f1f289cbb  assets/modules/modules.json
SUMS
)
```

`core.js` is the source input to the archived packer. The files under
`assets/modules/` are the corresponding packed replacements, with the
same core asset name and updated factory and manifest artifact IDs.
The APK prefix for these paths is `assets/flutter_assets/`. Each arm
installs its corresponding `base.js` or `manual.js` as
`assets/flutter_assets/assets/workload.js`. The two scripts differ
only in the `manualRelease` boolean.

## Reconstruct the Android build

The trial cleaned up its built engine, AOT and APKs. The archived hashes
identify the original run. A rebuild in a relocated workspace can produce
different binary hashes even with the same source and toolchain.

1. Use the synchronized Flutter checkout with its pinned Dart dependencies
   and the baseline GC patches. The selected engine target is
   `flax_android_release_arm64`, with Android arm64 V8 SDK 0.3.0
   (V8 15.4.80.15) verified against
   `engine/src/flutter/flax/source.lock.json` in the Flutter checkout.
   Rebuild `libflutter.so` in the configured Release target after applying
   `native.patch`. See P's `native-build.log` and `NATIVE_BUILD.sha256`,
   and the engine [build instructions](../../README.md).
2. Copy P's `app-build/commands.json` into a new build directory containing
   the reconstructed `app/`. Rebase the command paths to that workspace
   and the selected artifacts. The frontend is from `flax_mac_release_arm64`
   and uses the matching Android Release `flutter_patched_sdk`. Preserve
   the recorded package resolution and plugin registrant, including the
   archived Flax Dart package under
   `flax-staged-gc-20261106/android-ui/build/packages/flax`. Set
   `GEN_SNAPSHOT` to the archived compiler at
   `flax-joint-gc-worker-gate-20261107-1/native/r3/gen_snapshot` and verify
   its SHA-256 is `59a71a98c25899c977590de9ccf4a527e4898317bc28bc06b19ce7e179bd3d83`.
   With `P` pointing to the trial evidence and `BUILD` pointing to the
   relocated build directory, run both phases.

   ```sh
   python3 "$P/app-build/build_app.py" "$BUILD" kernel
   python3 "$P/app-build/build_app.py" "$BUILD" snapshot "$GEN_SNAPSHOT"
   ```

   The snapshot phase writes a fresh `AOT_READY.json` next to `libapp.so`
   with hashes for the current kernel, AOT, commands and compiler. Keep
   P's `AOT_READY.json` as the original build reference.
3. Replay P's `pack.py` in scratch with C8, the same rebuilt raw engine
   and AOT for both arms, and the corresponding generated workload and
   raw core. Its argument order is
   `RAW_ENGINE OUTPUT_LABEL LIBAPP WORKLOAD RAW_CORE`. The packer reads
   the fresh `AOT_READY.json` beside the rebuilt `libapp.so`. In the
   scratch copy of the packer, resolve C8 and tool inputs against the
   archived evidence tree, and set local signing and temporary paths.
   Keep the strip, align, signing and AOT compatibility checks. Check
   that the embedded Dart snapshot ID matches the rebuilt engine and
   the original ID `64fdb591cfc669db815d32a5e2ababc5`, and that the AOT
   feature flags match the recorded Android arm64 Release configuration.
4. Compare the two rebuilt APKs' non-signing entries. Only the workload
   entry should differ. Verify that the packed core, manifest and each
   arm's workload match the original asset hashes above. Record the
   rebuilt engine (raw and stripped) and AOT digests separately from P's
   `PAYLOAD_MANIFEST.json` and `package-*/PACKAGE.json`, which document
   the original run.

## Recorded result

All five lifetime cases passed on Pixel 4 in Android Release/AOT
(`joint-js`, `joint-dart`, `joint-none`, `joint-quiet`, `joint-busy`).
At 60 Hz, both UI orders shortened the longest Flutter callback gap and
longest screen gap, with zero explicit drops. Severe screen gaps (at least 100 ms)
went from 28 to 29 in the automatic-first order and from 28 to 26 in the
manual-first order. Only **one of two** qualified pairs passed the frozen
UI gate. The experiment is retained in isolation. See P's
`TRIAL.md`, `FUNCTIONAL.json`, and `UI_PAIRS.json` for the recorded trial.
