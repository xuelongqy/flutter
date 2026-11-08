// Isolated fixture fragment, inserted inside the original @flax/core factory.
const jointReleases = new WeakMap();

function unlinkJoint(ref) {
  const release = jointReleases.get(ref);
  if (release === undefined) throw new TypeError('Expected a local joint capability');
  if (release === null) return false;
  jointReleases.set(ref, null);
  try {
    release();
  } catch (error) {
    jointReleases.set(ref, release);
    throw error;
  }
  return true;
}

Object.defineProperty(module.exports, 'unlinkJoint', {value: unlinkJoint, enumerable: true});

function checkJoint(ok, message) {
  if (!ok) throw new Error('Joint fixture: ' + message);
}

function expectJointTypeError(operation) {
  try {
    operation();
  } catch (error) {
    if (error instanceof TypeError) return;
    throw error;
  }
  throw new Error('Joint fixture: expected TypeError');
}

if (typeof globalThis.__fixturePeerSetup === 'function') {
  Object.defineProperty(globalThis, '__takeUnlinkJointFixture', {
    configurable: true,
    value: () => {
      checkJoint(globalThis.__fixturePeerSetup() === true, 'native setup failed');
      const {
        __fixturePeerControl: peerControl,
        __fixturePeerBind: bindPeer,
        __fixturePeerRead: readPeer,
      } = globalThis;
      for (const name of [
        '__takeUnlinkJointFixture', '__fixturePeerSetup', '__fixturePeerControl',
        '__fixturePeerBind', '__fixturePeerRead',
      ]) delete globalThis[name];

      const {TextEditingController} = globalThis.__flaxModules.require('@flax/flutter/widgets');
      let independentAlias;

      function createObjectLease(id, alias) {
        let wrapper = peerControl(0);
        bindPeer(wrapper, id, ...(alias === undefined ? [] : [alias]));
        checkJoint(peerControl(1, wrapper) === true, 'missing leased native target');
        const view = Object.freeze({
          read() {
            if (jointReleases.get(view) === null) throw new TypeError('Closed joint view');
            return readPeer(id, wrapper);
          },
        });
        jointReleases.set(view, () => {
          checkJoint(peerControl(2, wrapper) === true, 'owned target removal failed');
          wrapper = null;
        });
        return {view, wrapper};
      }

      function checkObjectSafety() {
        const safetyId = -1;
        const ineligibleId = -2;
        const independentWrapper = peerControl(0);
        independentAlias = () => {
          checkJoint(peerControl(1, independentWrapper) === true, 'independent native target lost');
          return readPeer(safetyId);
        };
        const {view, wrapper} = createObjectLease(safetyId, independentWrapper);
        const alias = view;
        const unregistered = peerControl(0);
        const ordinary = Object.create(null);
        bindPeer(ordinary, ineligibleId);
        for (const invalid of [null, 42, [], ordinary, unregistered, wrapper, {...view}, new Proxy(view, {})]) {
          expectJointTypeError(() => unlinkJoint(invalid));
        }
        for (const invalid of [null, [], ordinary, unregistered, new Proxy(wrapper, {})]) {
          checkJoint(peerControl(2, invalid) === false, 'native accepted invalid owner');
        }
        checkJoint(readPeer(ineligibleId, ordinary) === ineligibleId, 'ineligible facade changed');
        checkJoint(view.read() === safetyId && alias.read() === safetyId, 'wrong live view');
        checkJoint(unlinkJoint(view) && !unlinkJoint(alias), 'object unlink was not idempotent');
        expectJointTypeError(() => view.read());
        expectJointTypeError(() => alias.read());
        checkJoint(peerControl(1, wrapper) === false, 'primary native target survived');
        checkJoint(peerControl(2, wrapper) === false, 'duplicate native close removed an edge');
        checkJoint(independentAlias() === safetyId, 'independent alias cannot dispatch');
        checkJoint(readPeer(safetyId, wrapper) === safetyId, 'reverse facade lost');
      }

      function registerListener(fn) {
        if (typeof fn !== 'function') throw new TypeError('Expected a fixture listener');
        const owner = TextEditingController({text: 'idle'});
        owner.addListener(fn);
        const token = Object.freeze(Object.create(null));
        jointReleases.set(token, () => owner.removeListener(fn));
        return {owner, token};
      }

      return {
        checkObjectSafety,
        createObjectLease: id => createObjectLease(id).view,
        registerListener,
        majorCount: () => peerControl(3),
        checkRecovery() {
          checkJoint(independentAlias() === -1, 'independent alias failed during natural recovery');
          independentAlias = null;
        },
      };
    },
  });
}
