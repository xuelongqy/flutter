import 'dart:convert';

import 'package:flax/bindings.dart';

const _items = <int>[3, 7];
int _twice(int value) => value * 2;

extension Values<T> on List<T> {
  (T, U) pair<U>(U other, {List<int> items = _items}) => (single, other);
  int count({List<int>? items = _items, int Function(int)? callback = _twice}) =>
      (items?.length ?? 0) + (callback?.call(3) ?? 0);
}

extension NullableWords on String? {
  int count({List<int> items = _items}) => (this?.length ?? 0) + items.length;
}

extension type Tagged<T>(T value) {
  T echo({List<int> items = _items}) => value;
}

var evaluations = 0;
List<String> receiver() {
  evaluations++;
  return ['receiver'];
}

dynamic wrongReceiver() => 3;
var evaluatedArgument = false;
List<int> argument() {
  evaluatedArgument = true;
  return [];
}

var checks = 0;
void check(bool value, String message) {
  checks++;
  if (!value) throw StateError(message);
}

void main() {
  final pair =
      flaxInvokeWithDefaults(Values<String>(receiver()).pair<int>, [7, null], [1], const [
            '',
            'items',
          ])
          as (String, int);
  check(pair == ('receiver', 7), 'Extension and method generic scopes');
  check(evaluations == 1, 'Bound receiver evaluated once');
  check(
    flaxInvokeWithDefaults(Values<String>(['a']).count, [null, null], [0], const [
          'items',
          'callback',
        ]) ==
        8,
    'Private collection and callback defaults',
  );
  check(
    flaxInvokeWithDefaults(Values<String>(['a']).count, [null, null], [3], const [
          'items',
          'callback',
        ]) ==
        0,
    'Explicit null remains provided',
  );
  check(
    flaxInvokeWithDefaults(NullableWords(null).count, [null], [0], const ['items']) == 2,
    'Nullable extension receiver',
  );
  check(
    flaxInvokeWithDefaults(Tagged<String>('tagged').echo, [null], [0], const ['items']) == 'tagged',
    'Extension type representation and generic owner',
  );
  var rejected = false;
  try {
    flaxInvokeWithDefaults(Values<String>(wrongReceiver()).count, [argument(), null], [1], const [
      'items',
      'callback',
    ]);
  } on TypeError {
    rejected = true;
  }
  check(rejected && !evaluatedArgument, 'Receiver check precedes argument effects');
  check(
    flaxInvokeWithDefaults(
          Presence('x').pick,
          [
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            77,
          ],
          [0x80000000],
          const [
            'v0',
            'v1',
            'v2',
            'v3',
            'v4',
            'v5',
            'v6',
            'v7',
            'v8',
            'v9',
            'v10',
            'v11',
            'v12',
            'v13',
            'v14',
            'v15',
            'v16',
            'v17',
            'v18',
            'v19',
            'v20',
            'v21',
            'v22',
            'v23',
            'v24',
            'v25',
            'v26',
            'v27',
            'v28',
            'v29',
            'v30',
            'v31',
          ],
        ) ==
        77,
    'Bound receiver shifts presence across a word boundary',
  );
  var rejectedMask = false;
  try {
    flaxInvokeWithDefaults(
      Presence('x').pick,
      [
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        null,
        77,
      ],
      [0x100000000],
      const [
        'v0',
        'v1',
        'v2',
        'v3',
        'v4',
        'v5',
        'v6',
        'v7',
        'v8',
        'v9',
        'v10',
        'v11',
        'v12',
        'v13',
        'v14',
        'v15',
        'v16',
        'v17',
        'v18',
        'v19',
        'v20',
        'v21',
        'v22',
        'v23',
        'v24',
        'v25',
        'v26',
        'v27',
        'v28',
        'v29',
        'v30',
        'v31',
      ],
    );
  } on ArgumentError {
    rejectedMask = true;
  }
  check(rejectedMask, 'Invalid high mask bits are checked before repacking');
  print(jsonEncode({'checks': checks, 'passed': true}));
}

extension Presence on String {
  int pick({
    int? v0 = 0,
    int? v1 = 1,
    int? v2 = 2,
    int? v3 = 3,
    int? v4 = 4,
    int? v5 = 5,
    int? v6 = 6,
    int? v7 = 7,
    int? v8 = 8,
    int? v9 = 9,
    int? v10 = 10,
    int? v11 = 11,
    int? v12 = 12,
    int? v13 = 13,
    int? v14 = 14,
    int? v15 = 15,
    int? v16 = 16,
    int? v17 = 17,
    int? v18 = 18,
    int? v19 = 19,
    int? v20 = 20,
    int? v21 = 21,
    int? v22 = 22,
    int? v23 = 23,
    int? v24 = 24,
    int? v25 = 25,
    int? v26 = 26,
    int? v27 = 27,
    int? v28 = 28,
    int? v29 = 29,
    int? v30 = 30,
    int? v31 = 31,
  }) => v31 ?? -100;
}
