import 'dart:convert';

import 'package:flax/bindings.dart';

const _defaults = <int>[3, 7];
int _twice(int value) => value * 2;

class Value<T> {
  Value({required this._item, this._items = _defaults, this._callback = _twice});

  final T _item;
  final List<int>? _items;
  final int Function(int) _callback;
}

void main() {
  var checks = 0;
  void check(bool value, String message) {
    checks++;
    if (!value) throw StateError(message);
  }

  final omitted = flaxInvokeWithDefaults(Value<String>.new, ['item', null, null], [1], const [
    'item',
    'items',
    'callback',
  ]) as Value<String>;
  check(omitted._item == 'item', 'Public name of required private formal');
  check(identical(omitted._items, _defaults), 'Private formal default');
  check(
    identical(omitted._callback, _twice) && omitted._callback(3) == 6,
    'Private callback default',
  );

  final supplied = flaxInvokeWithDefaults(
    Value<String>.new,
    ['supplied', null, _twice],
    [7],
    const ['item', 'items', 'callback'],
  ) as Value<String>;
  check(
    supplied._item == 'supplied' && supplied._items == null,
    'Explicit null on private nullable formal',
  );

  var rejected = false;
  try {
    flaxInvokeWithDefaults(Value<String>.new, [null, null, null], [0], const [
      'item',
      'items',
      'callback',
    ]);
  } on ArgumentError {
    rejected = true;
  }
  check(rejected, 'Required private formal must remain required');
  print(jsonEncode({'checks': checks, 'passed': true}));
}
