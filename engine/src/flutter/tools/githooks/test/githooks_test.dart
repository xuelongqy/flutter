// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:io' as io;

import 'package:githooks/githooks.dart';
import 'package:path/path.dart' as path;
import 'package:test/test.dart';

void main() {
  group('pre-push formatting scope', () {
    late io.Directory checkout;
    late io.File arguments;

    void git(List<String> args) {
      final io.ProcessResult result = io.Process.runSync(
        'git',
        args,
        workingDirectory: checkout.path,
      );
      expect(result.exitCode, 0, reason: '${result.stdout}\n${result.stderr}');
    }

    setUp(() {
      checkout = io.Directory.systemTemp.createTempSync('githooks push ');
      arguments = io.File(path.join(checkout.path, 'arguments.txt'));
      git(['init', '--initial-branch=topic']);
      git([
        '-c',
        'user.name=Hook test',
        '-c',
        'user.email=hook@example.invalid',
        'commit',
        '--allow-empty',
        '-m',
        'base',
      ]);
      final script = io.File(
        path.join(checkout.path, 'ci', io.Platform.isWindows ? 'format.bat' : 'format.sh'),
      );
      script.parent.createSync();
      script.writeAsStringSync(
        io.Platform.isWindows
            ? '@echo off\r\n>arguments.txt echo(%*\r\nif exist fail exit /b 1\r\nexit /b 0\r\n'
            : '#!/bin/sh\nprintf \'%s\\n\' "\$@" > arguments.txt\n[ ! -f fail ]\n',
      );
      if (!io.Platform.isWindows) {
        expect(io.Process.runSync('chmod', ['+x', script.path]).exitCode, 0);
      }
    });

    tearDown(() => checkout.deleteSync(recursive: true));

    test('uses the tracked fork branch instead of upstream main', () async {
      git(['remote', 'add', 'origin', '.']);
      git(['update-ref', 'refs/remotes/origin/flax/main', 'HEAD']);
      git(['branch', '--set-upstream-to=origin/flax/main']);
      final String base = io.Process.runSync('git', [
        'rev-parse',
        'HEAD',
      ], workingDirectory: checkout.path).stdout.toString().trim();
      expect(await run(['pre-push', '--flutter', checkout.path]), 0);
      expect(arguments.readAsStringSync().trim(), '--base=$base');
    });

    test('retains the formatter default without a tracking branch', () async {
      expect(await run(['pre-push', '--flutter', checkout.path]), 0);
      expect(arguments.readAsStringSync().trim(), isEmpty);
    });

    test('a formatting failure still blocks a push', () async {
      io.File(path.join(checkout.path, 'fail')).writeAsStringSync('');
      expect(await run(['pre-push', '--flutter', checkout.path]), 1);
    });
  });

  test('Fails gracefully without a command', () async {
    int? result;
    try {
      result = await run(<String>[]);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(1));
  });

  test('Fails gracefully with an unknown command', () async {
    int? result;
    try {
      result = await run(<String>['blah']);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(1));
  });

  test('Fails gracefully without --flutter', () async {
    int? result;
    try {
      result = await run(<String>['pre-push']);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(1));
  });

  test('Fails gracefully when --flutter is not an absolute path', () async {
    int? result;
    try {
      result = await run(<String>['pre-push', '--flutter', 'non/absolute']);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(1));
  });

  test('Fails gracefully when --flutter does not exist', () async {
    int? result;
    try {
      result = await run(<String>[
        'pre-push',
        '--flutter',
        if (io.Platform.isWindows) r'C:\does\not\exist' else '/does/not/exist',
      ]);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(1));
  });

  test('post-merge runs successfully', () async {
    int? result;
    try {
      final io.Directory flutterPath = io.File(io.Platform.script.path).parent.parent.parent;
      result = await run(<String>['post-merge', '--flutter', flutterPath.path]);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(0));
  });

  test('pre-rebase runs successfully', () async {
    int? result;
    try {
      final io.Directory flutterPath = io.File(io.Platform.script.path).parent.parent.parent;
      result = await run(<String>['pre-rebase', '--flutter', flutterPath.path]);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(0));
  });

  test('post-checkout runs successfully', () async {
    int? result;
    try {
      final io.Directory flutterPath = io.File(io.Platform.script.path).parent.parent.parent;
      result = await run(<String>['post-checkout', '--flutter', flutterPath.path]);
    } catch (e, st) {
      fail('Unexpected exception: $e\n$st');
    }
    expect(result, equals(0));
  });
}
