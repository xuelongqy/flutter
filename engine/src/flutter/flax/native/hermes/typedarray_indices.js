'use strict';

function check(value, message) {
  if (!value) throw new Error(message);
}

let checked = 0;
for (const Type of [
  Uint8ClampedArray, Uint8Array, Int8Array, Uint16Array, Int16Array,
  Uint32Array, Int32Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array,
]) {
  const bigint = Type === BigInt64Array || Type === BigUint64Array;
  const array = new Type(2);
  check(ArrayBuffer.isView(array), 'native view identity');
  for (const key of [-1, '-1', -0.5, '-0.5', '-0', NaN, 'NaN', Infinity,
    '-Infinity', 2, '2', 4294967295, '9007199254740992']) {
    let converted = 0;
    const value = {valueOf() { converted++; return bigint ? 1n : 1; }};
    array[key] = value;
    check(converted === 1, 'ignored stores still convert their value: ' + key);
    check(array[key] === undefined && !(key in array), 'invalid read: ' + key);
    check(!Object.prototype.hasOwnProperty.call(array, key), 'invalid own key: ' + key);
    check(Reflect.defineProperty(array, key, {value: 1}) === false, 'invalid descriptor: ' + key);
    let threw = false;
    try { Object.defineProperty(array, key, {value: 1}); }
    catch (error) { threw = error instanceof TypeError; }
    check(threw, 'throwing invalid descriptor: ' + key);
    const receiver = {};
    check(Reflect.set(array, key, value, receiver), 'invalid receiver store');
    check(converted === 1 && Object.keys(receiver).length === 0, 'invalid receiver side effects');
    check(delete array[key], 'invalid deletion');
    checked++;
  }
  array[-0] = bigint ? 1n : 1;
  check(array[0] === (bigint ? 1n : 1), 'numeric negative zero');
  for (const key of ['foo', '01', '1.0', '1e0', '-1.0', '+1', Symbol('pixel')]) {
    array[key] = 'ordinary';
    check(array[key] === 'ordinary', 'ordinary property');
  }
  // Constant string keys can use named-property bytecodes instead of computed ones.
  array['-1'] = bigint ? 1n : 1;
  check(array['-1'] === undefined, 'constant negative index');
  const parent = Object.create(Object.getPrototypeOf(array));
  parent['-1'] = 99;
  Object.setPrototypeOf(array, parent);
  check(array['-1'] === undefined && !('-1' in array), 'no prototype fallback');
}

const ordinary = [];
ordinary[-1] = 9;
check(ordinary[-1] === 9, 'ordinary arrays keep negative properties');
print('TypedArray numeric index checks passed: ' + checked);
