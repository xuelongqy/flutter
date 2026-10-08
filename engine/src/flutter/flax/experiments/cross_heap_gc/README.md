# Cross-heap GC experiment

Experimental proof on macOS arm64, not a production Flutter engine integration.
The local fork starts at Flutter **3.47.6 stable**, commit
`5fc346839b5d0eef006ed8404392afb4dfae428d`, with its pinned Dart **3.13.5**
revision `04bcd1036cdc799ac6564988f159ee454d42c822`.
The platform direction is Hermes on iOS and V8 with JIT on other native platforms.
Only macOS arm64 V8 is exercised here.

## What passed

**20/20 real runtime cases**, with both the custom DEBUG VM (marking and heap
verification enabled) and the optimized custom VM. The existing V8 SDK
`0.3.0 / V8 15.4.80.15 macos-arm64` was reused. A JIT code event handler observed
native code for the probe function; V8 was not run with `--jitless`.

The cases use real Dart objects, closures, weak references and moving collections,
not projected or shadow Dart objects:

1. Ordinary JS closure, private-field and live-key WeakMap edges, each with Dart,
   JS, both or neither as the remaining business owner: 12 cases.
2. Twelve-object cross-heap chains under the same four owner combinations: 4 cases.
3. A JS WeakRef-only edge must not retain the Dart target: 1 case.
4. JS to Dart callback reentry through GC, automatic Dart allocation GC without a
   forced collection, and closing JS while a Dart owner survives: 3 cases.

A remaining owner preserved identity and invocation; dropping all business owners
collected the Dart target, Dart callback origin and native peer. Four minor GCs
per ownership case conservatively retained bridge objects before joint major GCs.
The automatic allocation case observed a real joint major collection.

**26/26 selected unmodified upstream VM regressions passed**, covering full GC,
weak handles, weak properties/references/arrays and finalizers. An initial run was
25/26 because `DartAPI_WeakPersistentHandleErrors` compiles Dart source and needs
`--dfe`. Supplying the source-built `gen/kernel_service.dill` resolved that harness
error; all 26 were rerun with it. No assertion was removed or weakened.
The probe analyzer reported no errors or warnings and 39 style-level infos under
Flutter's analysis rules. This does not certify the full Flutter or Flax suites.

## How reachability is shared

The Dart patch registers weak bridge handles and records which Dart objects own
JS callbacks. Minor collections keep registered objects alive. At a major GC
safepoint, ordinary Dart marking finishes first; marked callback origins seed
native peers as conditional roots in the public V8 `CppHeap`.

V8 performs its ordinary GC. A reachable JS wrapper traces its native peer;
that peer marks its Dart target through the restricted experimental marker API.
Dart drains newly discovered objects and reports newly marked callback origins,
which join V8/CppHeap marking. This reaches a joint fixed point before weak
finalization. A rootless cycle supplies no initial mark and is collected.

There is no additional JS heap scanner, heap snapshot, shadow object, or V8 source
patch. This reuses V8's native CppHeap tracing. It still includes V8's normal heap
traversal and Dart's normal marking work.

`dart.patch` changes six tracked Dart files and adds one private API header. It
also adds the missing existing zlib dependency to Flutter's upstream VM test
build target. No toolchain lock or SDK release asset was changed.

## Measured pauses

The optimized VM measured 1,000 live bridge peers with the ordinary JS objects
below retained. Each GC column uses 12 wall-clock samples after warmup, with
verification outside the timer. P50 is the upper middle sample for an even-sized
sample set; max is the largest observed sample. The final run had no concurrent
task-owned compiler or probe workload.

| Live ordinary JS objects | V8-only P50 / max | Joint Dart + V8 P50 / max |
| ---: | ---: | ---: |
| 0 | 0.372 / 0.525 ms | 2.501 / 3.129 ms |
| 100,000 | 1.167 / 1.888 ms | 3.395 / 4.216 ms |
| 300,000 | 4.313 / 5.302 ms | 6.229 / 6.843 ms |

Joint measurements include Dart GC, V8 GC and the bridge, not just the added
tracing hook. The two columns are separate measurements, not an isolated estimate
of integration overhead. These small synthetic heaps do not establish a latency
bound, compare directly to old projection benchmarks, or prove Flutter frame times.

## Reproduce locally

Start from the synchronized fork and its pinned `gclient` dependencies. Run
`gclient runhooks` in the Flutter checkout before building. From this checkout:

```sh
cd /Volumes/MAC_HOME/Develop/Flutter/flutter
python3 engine/src/flutter/flax/experiments/cross_heap_gc/apply_dart_patch.py
python3 engine/src/flutter/flax/experiments/cross_heap_gc/build_vm.py
python3 engine/src/flutter/flax/experiments/cross_heap_gc/build_vm.py --optimized
cmake -S engine/src/flutter/flax/experiments/cross_heap_gc \
  -B .local/gc-probe-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDART_SOURCE="$PWD/engine/src/flutter/third_party/dart" \
  -DV8_SDK=/Volumes/MAC_HOME/Develop/Flutter/flax/.local/ci-ca7f98f/sdk-36954561632-verified/v8-macos-arm64
cmake --build .local/gc-probe-build -j4
FLAX_GC_PROBE="$PWD/.local/gc-probe-build/libflax_joint_gc_probe.dylib" \
  engine/src/out/flax_gc_debug/dartvm --verify_after_marking --verify_after_gc \
  engine/src/flutter/flax/experiments/cross_heap_gc/probe.dart
FLAX_GC_PROBE="$PWD/.local/gc-probe-build/libflax_joint_gc_probe.dylib" \
  engine/src/out/flax_gc_optimized/dartvm \
  engine/src/flutter/flax/experiments/cross_heap_gc/probe.dart --bench
ninja -C engine/src/out/flax_gc_debug -j4 run_vm_tests
engine/src/out/flax_gc_debug/run_vm_tests \
  --dfe="$PWD/engine/src/out/flax_gc_debug/gen/kernel_service.dill" \
  DartAPI_WeakPersistentHandleErrors
```

The patch helper refuses a different Dart revision and recognizes an already
applied patch. The build helper uses the pinned clang compiler. For this machine's
Xcode 27 TAPI compatibility, it changes only generated Ninja linker commands to
use the installed Apple linker; rerunning GN overwrites that local adjustment.
No shared Flutter installation or locked toolchain is replaced.

## Ceilings before application integration

- Joint major marking is serial and CppHeap marking/sweeping is atomic. Larger
  Dart heaps and real Flutter frame pauses need measurement.
- One collector per Dart isolate group is exercised. Multiple FlaxViews/runtimes
  and isolate migration are untested. A major GC on another Dart thread retains
  registered handles conservatively instead of entering V8 there.
- Dart allocation can start a joint GC. JS-only pressure does not yet request a
  Dart joint major; V8-only GC retains conditional bridge roots until that major.
- Native diagnostic cells and weak handles remain until probe close so tests can
  inspect their state. Long-running metadata reclamation is not proven.
- Production Flax reference tables, Widget lifecycles, Flutter embedding/AOT and
  iOS Hermes or other platforms are not integrated or validated here.

Local logs, exact commands, sample arrays and preservation hashes are in
`/Volumes/MAC_HOME/Develop/Flutter/flax/.local/gc-flutter-engine/`.
Existing Flax dirty files were preserved. No commit, push, CI or SDK rebuild was
performed. The next experiment should connect the same conditional tracing edges
to real Flax references in a macOS Flutter application and measure widget teardown
and frame pauses before extending platforms.
