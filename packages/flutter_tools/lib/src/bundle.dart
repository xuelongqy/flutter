// Copyright 2014 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'package:convert/convert.dart';
import 'package:crypto/crypto.dart';

import 'artifacts.dart';
import 'base/config.dart';
import 'base/file_system.dart';
import 'build_info.dart';
import 'compile.dart';
import 'convert.dart';
import 'globals.dart' as globals;

String get defaultMainPath => globals.fs.path.join('lib', 'main.dart');
const defaultManifestPath = 'pubspec.yaml';
String get defaultDepfilePath => globals.fs.path.join(getBuildDirectory(), 'snapshot_blob.bin.d');

String getDefaultApplicationKernelPath({required bool trackWidgetCreation}) {
  return getKernelPathForTransformerOptions(
    globals.fs.path.join(getBuildDirectory(), 'app.dill'),
    trackWidgetCreation: trackWidgetCreation,
  );
}

String getDefaultCachedKernelPath({
  required bool trackWidgetCreation,
  required List<String> dartDefines,
  required Config config,
  required FileSystem fileSystem,
  TargetModel? targetModel,
  List<String> extraFrontEndOptions = const <String>[],
  Artifacts? artifacts,
}) {
  final buffer = StringBuffer();
  // Local SDKs can change without changing the Flutter revision or their paths.
  if (artifacts?.usesLocalArtifacts ?? false) {
    for (final artifact in const <Artifact>[
      Artifact.frontendServerSnapshotForEngineDartSdk,
      Artifact.platformKernelDill,
    ]) {
      final File input = fileSystem.file(artifacts!.getArtifactPath(artifact));
      buffer.write('$artifact:${sha256.convert(input.readAsBytesSync())};');
    }
  }
  final List<String> cacheFrontEndOptions = extraFrontEndOptions.toList()
    ..removeWhere((String arg) => arg.startsWith('--enable-experiment='));
  if (targetModel != null) {
    buffer.write('$targetModel;');
  }
  buffer.writeAll(dartDefines);
  buffer.writeAll(cacheFrontEndOptions);
  var buildPrefix = '';
  if (buffer.isNotEmpty) {
    final output = buffer.toString();
    final Digest digest = md5.convert(utf8.encode(output));
    buildPrefix = '${hex.encode(digest.bytes)}.';
  }
  return getKernelPathForTransformerOptions(
    fileSystem.path.join(getBuildDirectory(config, fileSystem), '${buildPrefix}cache.dill'),
    trackWidgetCreation: trackWidgetCreation,
  );
}

String getKernelPathForTransformerOptions(String path, {required bool trackWidgetCreation}) {
  if (trackWidgetCreation) {
    path += '.track.dill';
  }
  return path;
}
