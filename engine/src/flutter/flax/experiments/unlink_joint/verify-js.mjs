// Node/V8 acceptance of the real core asset and workloads with fixture host doubles.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {performance} from 'node:perf_hooks';
import {createContext, runInContext, SourceTextModule, SyntheticModule} from 'node:vm';

const [template, generated] = process.argv.slice(2);
assert.equal(process.argv.length, 4,
  'Usage: node --experimental-vm-modules verify-js.mjs TEMPLATE_APK GENERATED_DIR');
const extract = asset => execFileSync('unzip', ['-p', template,
  'assets/flutter_assets/assets/' + asset], {encoding: 'utf8'});
const registry = extract('modules/registry.js');
const originalCore = extract('modules/_flax_core-14fe61593557.js');
const read = name => readFileSync(join(generated, name), 'utf8');
const core = read('core.js');
const base = read('base.js');
const manual = read('manual.js');
const fragment = readFileSync(new URL('unlink-joint.js', import.meta.url), 'utf8');
assert.equal(core.replace(fragment + '\n', ''), originalCore);
assert.equal(base.replace('const manualRelease = false;', 'const manualRelease = true;'), manual);

function setup(source = core, fixture = true) {
  const context = createContext({performance});
  const counters = {objectsRemoved: 0, listenersAdded: 0, listenersRemoved: 0, calls: 0};
  const faults = {beforeRemove: null, rejectRemove: false};
  const wrappers = new WeakSet();
  const nativeTargets = new WeakSet();
  const originals = new Map();
  if (fixture) Object.assign(context, {
    __fixturePeerSetup: () => true,
    __fixturePeerControl(op, ref) {
      if (op === 0) {
        const wrapper = Object.create(null);
        wrappers.add(wrapper);
        return wrapper;
      }
      if (op === 3) return 23;
      if (!nativeTargets.has(ref)) return false;
      if (op === 2) {
        if (faults.rejectRemove) return false;
        faults.beforeRemove?.();
        assert(nativeTargets.delete(ref));
        ++counters.objectsRemoved;
      }
      return true;
    },
    __fixturePeerBind(ref, id, alias) {
      assert(!originals.has(id));
      originals.set(id, ref);
      if (wrappers.has(ref)) nativeTargets.add(ref);
      if (alias !== undefined) nativeTargets.add(alias);
    },
    __fixturePeerRead(id, ref) {
      assert(originals.has(id));
      if (ref !== undefined) assert.equal(ref, originals.get(id));
      return id;
    },
  });

  const widgets = {
    ListView: {builder() {}},
    ScrollController: () => ({}),
    Text() {},
    runApp() {},
    TextEditingController({text}) {
      assert.equal(new.target, undefined);
      const listeners = new Set();
      return {
        get text() { return text; },
        set text(value) {
          if (text === value) return;
          text = value;
          for (const fn of listeners) {
            ++counters.calls;
            fn();
          }
        },
        get hasListeners() { return listeners.size > 0; },
        addListener(fn) {
          assert(!listeners.has(fn));
          listeners.add(fn);
          ++counters.listenersAdded;
        },
        removeListener(fn) {
          assert(listeners.delete(fn));
          ++counters.listenersRemoved;
        },
      };
    },
  };

  runInContext(registry, context, {filename: 'template/registry.js'});
  runInContext(source, context, {filename: 'core.js'});
  const root = JSON.parse(source.split(', function(module, exports, require)')[0]
    .slice('globalThis.__flaxModules.define('.length));
  const bindings = {
    specifier: '@flax/core/bindings', owner: 'mock:bindings', version: '0.0.0',
    artifact: 'mock:bindings', dependencies: {},
  };
  const widget = {
    specifier: '@flax/flutter/widgets', owner: 'mock:widgets', version: '0.0.0',
    artifact: 'mock:widgets', dependencies: {},
  };
  context.__flaxModules.define(bindings, () => {});
  context.__flaxModules.define(widget, module => { module.exports = widgets; });
  context.__flaxModules.seal([root, bindings, widget]);
  const coreExports = context.__flaxModules.require('@flax/core');

  async function evaluate(entry) {
    const namespaces = new Map([
      ['@flax/core', coreExports], ['@flax/flutter/widgets', widgets],
      ['@flax/dart/core', {Duration() {}}], ['@flax/flutter/animation', {Curves: {}}],
    ]);
    const mod = new SourceTextModule(entry, {context});
    await mod.link(specifier => {
      const values = namespaces.get(specifier);
      assert(values, specifier);
      return new SyntheticModule(Object.keys(values), function () {
        for (const [key, value] of Object.entries(values)) this.setExport(key, value);
      }, {context, identifier: specifier});
    });
    await mod.evaluate();
    return mod.namespace;
  }

  return {context, coreExports, counters, faults, evaluate};
}

const original = setup(originalCore, false);
const ordinary = setup(core, false);
assert.deepEqual(Object.keys(ordinary.coreExports), [...Object.keys(original.coreExports), 'unlinkJoint']);
assert.equal(ordinary.context.__takeUnlinkJointFixture, undefined);
const {unlink} = await ordinary.evaluate("export {unlinkJoint as unlink} from '@flax/core';");
assert.equal(unlink, ordinary.coreExports.unlinkJoint);

const local = setup();
const foreign = setup();
const localFixture = local.context.__takeUnlinkJointFixture();
const foreignFixture = foreign.context.__takeUnlinkJointFixture();
const view = localFixture.createObjectLease(0);
const foreignView = foreignFixture.createObjectLease(0);
const foreignToken = foreignFixture.registerListener(() => {}).token;
for (const invalid of [foreignView, foreignToken]) {
  assert.throws(() => local.coreExports.unlinkJoint(invalid), {name: 'TypeError'});
}
local.faults.rejectRemove = true;
assert.throws(() => local.coreExports.unlinkJoint(view), /owned target removal failed/);
assert.equal(view.read(), 0);
local.faults.rejectRemove = false;
local.faults.beforeRemove = () => {
  assert.equal(local.coreExports.unlinkJoint(view), false);
  assert.throws(() => view.read(), {name: 'TypeError'});
};
assert.equal(local.coreExports.unlinkJoint(view), true);
assert.equal(local.coreExports.unlinkJoint(view), false);
assert.throws(() => view.read(), {name: 'TypeError'});
assert.equal(local.counters.objectsRemoved, 1);
console.log('PASS original exports + ESM named import, cross-context rejection, failed removal retry and reentrant closure');

for (const [source, treatment] of [[base, false], [manual, true]]) {
  const {context, counters, evaluate} = setup();
  await evaluate(source);
  const report = JSON.parse(context.__prepareManualPeers());
  assert.equal(report.manual, treatment);
  assert.equal(Object.keys(report.checks).length, 4);
  assert(Object.values(report.checks).every(Boolean));
  assert.equal(counters.objectsRemoved, 1);
  assert.equal(counters.listenersAdded, 2);
  assert.equal(counters.listenersRemoved, 2);
  assert.equal(counters.calls, 2);
  for (let batch = 0; batch < 50; ++batch) {
    const [removed, objectUs, majors, calls, unlinked, callbackUs] =
      JSON.parse(context.__retireManualPeers());
    assert.equal(removed, treatment ? 20 : 0);
    assert(Number.isInteger(objectUs) && objectUs >= 0);
    assert.equal(majors, 0);
    assert.equal(calls, 1);
    assert.equal(unlinked, Number(treatment));
    assert(Number.isInteger(callbackUs) && callbackUs >= 0);
  }
  assert.equal(context.__checkManualPeerRecovery(), true);
  assert.deepEqual(counters, {
    objectsRemoved: treatment ? 1001 : 1, listenersAdded: 52,
    listenersRemoved: treatment ? 52 : 2, calls: 52,
  });
  console.log(`PASS ${treatment ? 'manual' : 'base'} ESM workload, 1000 objects, 50 listener cycles, 50 measured callbacks`);
}
