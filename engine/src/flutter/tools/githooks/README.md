# Git Hooks

The behavior of `git` commands can be customized through the use of "hooks".
These hooks are described in detail in git's
[documentation](https://git-scm.com/book/en/v2/Customizing-Git-Git-Hooks).

`git` looks for an executables by name in the directory specified by
the `core.hooksPath` `git config` setting. The script `setup.py` here points
`core.hooksPath` at this directory. It runs during a `gclient sync` or a
`gclient runhooks`.

The hooks here are implemented in Dart by the program with
entrypoint `bin/main.dart` in this directory. The commands of the program
are the implementation of the different hooks, for example
`bin/main.dart pre-push ...`. Since the Dart program itself isn't an executable,
these commands are invoked by small Python wrapper scripts. These wrapper
scripts have the names that `git` will look for.
The pre-push wrapper uses standard Python 3 and needs no `vpython3` on PATH.

## pre-push

This hooks runs when pushing commits to a remote branch, for example to
create or update a pull request: `git push origin my-local-branch`.

The `pre-push` hook runs `ci/format.sh`. It checks Dart, C++/Objective-C/shader,
GN, Java and Python formatting, trailing whitespace and header guards. Clang-tidy
is optional and enabled with `PRE_PUSH_CLANG_TIDY`; analysis and license checks
are not run.

When the current branch has a remote-tracking upstream, the hook passes that
revision as the formatter's `--base`. For a maintained fork such as `flax/main`,
this checks changes since `origin/flax/main` instead of the entire fork history
since Flutter's upstream `main`. The hook uses the local tracking ref; ordinary
fetches and successful pushes keep it current. Without a tracking branch, the
formatter retains its original upstream-main fallback.

Formatting still checks the working tree, including staged and unstaged changes.
Run the same scope manually with `ci/format.sh --base <git-ref>`. An explicit base
does not fetch from the network; an unknown revision or failed check blocks the
push. No formatter check is disabled for maintained branches.

### Adding new pre-push checks

Since the pre-push checks run on every `git push`, they should run quickly.
New checks can be added by modifying the `run()` method of the `PrePushCommand`
class in `lib/src/pre_push_command.dart`.

## Creating a new hook

1. Check the `git` documentation, and copy `pre-push` into a script with
the right name.
1. Make sure the script has the executable bit set
(`chmod +x <script>`).
1. Add a new `Command` implementation under `lib/src`. Give the new
`Command` the same name as the new hook.
1. Add the new `Command` to the `CommandRunner` in `lib/githooks.dart`.
1. Make sure the script from step (1) is passing the new command to the Dart
program.
