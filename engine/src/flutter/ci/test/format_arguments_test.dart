// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io';

import 'package:engine_repo_tools/engine_repo_tools.dart';
import 'package:test/test.dart';

void main() {
  final Directory engineRoot = Engine.findWithin().flutterDir;
  Future<ProcessResult> format(List<String> args) => Process.run(Platform.resolvedExecutable, [
    File('${engineRoot.path}/ci/bin/format.dart').path,
    ...args,
  ], workingDirectory: engineRoot.path);

  test('an explicit base runs checks without selecting upstream main', () async {
    final ProcessResult result = await format(['--base=HEAD', '--check=whitespace']);
    expect(result.exitCode, 0, reason: '${result.stdout}\n${result.stderr}');
    expect(result.stdout, contains('whitespace'));
  });

  test('invalid arguments and missing base revisions fail closed', () async {
    for (final args in [
      ['--not-a-format-option'],
      ['--base=refs/heads/nonexistent-format-test-ref', '--check=whitespace'],
    ]) {
      final ProcessResult result = await format(args);
      expect(result.exitCode, isNot(0), reason: '${result.stdout}\n${result.stderr}');
    }
  });
}
