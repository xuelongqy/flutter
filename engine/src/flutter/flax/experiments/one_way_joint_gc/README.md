# One-way joint GC experiment

The Android arm64 Release/AOT experiment is available in a separate engine
checkout. The maintained build uses the existing joint collector. The
experimental checkout pins the staged VM, the one-way collector and request
batching together.

| Revision | Experiment |
| --- | --- |
| `f8f6599d64c` | Dart posts a confirmation request and returns. Flax joins an eligible V8 major or attempts a deadline collection. |
| `88ee8962d2d` | Coalesce requests while a confirmation is pending. |
| `d52f02fc07c` | Preserve the separate [unlinkJoint fixture](../unlink_joint/README.md). |

From the engine repository, create a disposable worktree on the workspace
volume. Follow the [engine setup](../../README.md) for dependency sync and
the pinned V8 Android SDK, then build the experiment.

```sh
git worktree add --detach "$EXPERIMENT_ENGINE" d52f02fc07c0e598c43e13b6bdd84be67c66cdb4
cd "$EXPERIMENT_ENGINE"
python3 engine/src/flutter/flax/tools/build_engine.py \
  --sdk "$V8_ANDROID_SDK" --target android-arm64 --mode release
```

In the experimental build, call the private
`flax_experiment_incremental_joint(1)` entry on the owning UI isolate after
creating the runtime and before starting the workload. A return of `1`
confirms selection. Mode `0` selects the staged control. The
[unlinkJoint fixture](../unlink_joint/README.md) has its own replay recipe.

The Android trial permits at most one additional eligible Dart isolate in the
group. Persistent denial can postpone cycle reclamation. Turning on one-way
by default in a macOS test build failed eight existing GC lifetime contracts.
The staged control also increased both longest screen gaps in a matched
Pixel 4 comparison against the maintained engine. These results limit the
changes to the experiment checkout.
