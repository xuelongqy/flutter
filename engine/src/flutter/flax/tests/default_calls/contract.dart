import 'dart:convert';
import 'package:flax/bindings.dart';

const _privateList = <int>[3, 7];
int _callback(int value) => value * 2;

class Value<T> {
  Value(this.item, {this.items = _privateList, this.callback = _callback});
  final T item;
  final List<int> items;
  final int Function(int) callback;
  T echo(T value, {List<int> items = _privateList}) => value;
  List<U> wrap<U>(U value, {List<int> items = _privateList}) => [value];
  T mixed<U>(T value, U other, {List<int> items = _privateList}) => value;
  U bounded<U extends Comparable<U>>(U value, {List<int> items = _privateList}) => value;
  int positional([int a = 1, int b = 2]) => a + b;
  int score({int a = 3}) => a;
}

class Ordered<T extends Comparable<T>> {
  Ordered(this.value, {this.items = _privateList});
  final T value;
  final List<int> items;
}

class Derived extends Value<String> {
  Derived() : super('derived');
  @override
  int score({int a = 9, int extra = 10}) => a + extra;
  @override
  int positional([int a = 4, int b = 5, int extra = 6]) => a + b + extra;
  int base() => flaxInvokeWithDefaults(super.score, [null], [0], const ['a']) as int;
}

abstract class Alias<T> {
  factory Alias(T item, {List<int> items}) = AliasHop<T>;
  T get item;
  List<int> get items;
}

abstract class AliasHop<T> implements Alias<T> {
  factory AliasHop(T item, {List<int> items}) = AliasImpl<T>;
}

class AliasImpl<T> implements AliasHop<T> {
  AliasImpl(this.item, {this.items = _privateList});
  @override
  final T item;
  @override
  final List<int> items;
}

var evaluations = 0;
Value<String> receiver() {
  evaluations++;
  return Derived();
}

var checks = 0;
void check(bool value, String message) {
  checks++;
  if (!value) throw StateError(message);
}

void reject(void Function() run) {
  var rejected = false;
  try {
    run();
  } on ArgumentError {
    rejected = true;
  } on TypeError {
    rejected = true;
  }
  check(rejected, 'Invalid call accepted');
}

void main() {
  final alias =
      flaxInvokeWithDefaults(
            Alias<List<String>>.new,
            [
              <String>['factory'],
              null,
            ],
            [1],
            const ['', 'items'],
          )
          as Alias<List<String>>;
  check(alias.item.single == 'factory', 'Nested generic redirect factory chain');
  check(identical(alias.items, _privateList), 'Redirect factory private default');
  final value =
      flaxInvokeWithDefaults(Value<String>.new, ['item', null, null], [1], const [
            '',
            'items',
            'callback',
          ])
          as Value<String>;
  check(
    value.item == 'item' &&
        identical(value.items, _privateList) &&
        identical(value.callback, _callback),
    'Private defaults',
  );
  check(value.callback(3) == 6, 'Callback default');
  check(
    flaxInvokeWithDefaults(value.echo, ['next', null], [1], const ['', 'items']) == 'next',
    'Generic owner',
  );
  check(
    (flaxInvokeWithDefaults(value.wrap<int>, [7, null], [1], const ['', 'items']) as List<int>)
            .single ==
        7,
    'Generic method',
  );
  check(
    flaxInvokeWithDefaults((value as Value<Object?>).mixed<int>, ['mixed', 7, null], [3], const [
          '',
          '',
          'items',
        ]) ==
        'mixed',
    'Class and method generic scopes',
  );
  check(
    flaxInvokeWithDefaults(value.bounded<String>, ['bounded', null], [1], const ['', 'items']) ==
        'bounded',
    'Recursive method bound',
  );
  final ordered =
      flaxInvokeWithDefaults(Ordered<String>.new, ['ordered', null], [1], const ['', 'items'])
          as Ordered<String>;
  check(
    ordered.value == 'ordered' && identical(ordered.items, _privateList),
    'Recursive constructor bound',
  );
  check(
    flaxInvokeWithDefaults(receiver().score, [null], [0], const ['a']) == 19,
    'Override defaults',
  );
  check(evaluations == 1, 'Receiver evaluated twice');
  check(Derived().base() == 3, 'Lexical super default');
  check(
    flaxInvokeWithDefaults(value.positional, [null, null], [0], const ['', '']) == 3,
    'Omitted positional',
  );
  check(
    flaxInvokeWithDefaults(value.positional, [10, null], [1], const ['', '']) == 12,
    'Trailing positional',
  );
  check(
    flaxInvokeWithDefaults(receiver().positional, [null, null], [0], const ['', '']) == 15,
    'Extra override positional default',
  );
  reject(() => flaxInvokeWithDefaults(value.positional, [null, 10], [2], const ['', '']));
  reject(
    () => flaxInvokeWithDefaults(Value<String>.new, [null, null, null], [0], const [
      '',
      'items',
      'callback',
    ]),
  );
  reject(() => flaxInvokeWithDefaults(value.score, ['bad'], [1], const ['a']));
  reject(() => flaxInvokeWithDefaults(value.score, [1], [2], const ['a']));
  check(
    flaxInvokeWithDefaults('x'.padLeft, [3, null], [1], const ['', '']) == '  x',
    'SDK virtual entry',
  );
  print(jsonEncode({'checks': checks, 'passed': true, 'evaluations': evaluations}));
}
