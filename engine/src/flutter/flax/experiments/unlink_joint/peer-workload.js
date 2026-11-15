const manualRelease = false;
const leaseCount = 1000;
const leases = new Array(leaseCount);
let nextLease = 0;
let jointFixture;

function checkPeer(ok, message) {
  if (!ok) throw new Error('Joint workload: ' + message);
}

function checkListenerSafety() {
  const calls = [];
  let active;
  function listener() {
    const {owner, token} = active;
    if (active === a) {
      calls.push('A enter');
      checkPeer(owner.text === 'first', 'A saw unexpected state');
      checkPeer(flax.unlinkJoint(token) && !flax.unlinkJoint(token), 'A unlink failed or repeated');
      calls.push('A exit');
    } else {
      checkPeer(owner.text === 'other', 'B saw unexpected state');
      calls.push('B');
    }
  }
  const a = jointFixture.registerListener(listener);
  const b = jointFixture.registerListener(listener);
  for (const invalid of [listener, a.owner, Object.create(null), new Proxy(a.token, {})]) {
    let rejected = false;
    try {
      flax.unlinkJoint(invalid);
    } catch (error) {
      if (!(error instanceof TypeError)) throw error;
      rejected = true;
    }
    checkPeer(rejected, 'accepted an ineligible listener reference');
  }
  active = a;
  a.owner.text = 'first';
  a.owner.text = 'later';
  checkPeer(!flax.unlinkJoint(a.token) && !a.owner.hasListeners, 'A registration survived');
  active = b;
  b.owner.text = 'other';
  checkPeer(calls.join(',') === 'A enter,A exit,B' && b.owner.hasListeners,
    'same-function listener order or independence failed: ' + calls);
  checkPeer(flax.unlinkJoint(b.token), 'B cleanup failed');
}

globalThis.__prepareManualPeers = () => {
  jointFixture = globalThis.__takeUnlinkJointFixture();
  checkPeer(typeof globalThis.__takeUnlinkJointFixture === 'undefined', 'fixture setup is reusable');
  jointFixture.checkObjectSafety();
  checkListenerSafety();
  for (let i = 0; i < leaseCount; ++i) leases[i] = jointFixture.createObjectLease(i);
  return JSON.stringify({
    manual: manualRelease,
    checks: {
      objectAliasesAndReverseFacade: true,
      nativeTargetOnly: true,
      rejectsIneligible: true,
      sameFunctionSelfUnlink: true,
    },
  });
};

globalThis.__retireManualPeers = () => {
  checkPeer(nextLease + 20 <= leaseCount, 'too many retirement batches');
  const limit = nextLease + 20;
  for (let id = nextLease; id < limit; ++id)
    checkPeer(leases[id].read() === id, 'Dart value lost before last use');
  const majorBefore = jointFixture.majorCount();
  const begin = performance.now();
  for (; nextLease < limit; ++nextLease) {
    if (manualRelease) checkPeer(flax.unlinkJoint(leases[nextLease]), 'lease closed early');
    leases[nextLease] = null;
  }
  const elapsedUs = Math.round((performance.now() - begin) * 1000);
  const majorAfter = jointFixture.majorCount();
  checkPeer(Number.isSafeInteger(majorBefore) && Number.isSafeInteger(majorAfter),
    'V8 major counter unavailable');

  let callbackCalls = 0;
  const {owner, token} = jointFixture.registerListener(() => {
    checkPeer(owner.text === 'batch', 'listener owner lost before last use');
    ++callbackCalls;
  });
  owner.text = 'batch';
  const callbackBegin = performance.now();
  const released = manualRelease && flax.unlinkJoint(token);
  const callbackReleaseUs = Math.round((performance.now() - callbackBegin) * 1000);
  checkPeer(callbackCalls === 1 && released === manualRelease, 'wrong callback work');
  checkPeer(owner.hasListeners === !manualRelease, 'wrong listener registration state');
  return JSON.stringify([
    manualRelease ? 20 : 0, elapsedUs, majorAfter - majorBefore,
    callbackCalls, Number(released), callbackReleaseUs,
  ]);
};

globalThis.__checkManualPeerRecovery = () => {
  checkPeer(nextLease === leaseCount, 'workload leases remain open');
  jointFixture.checkRecovery();
  return true;
};
