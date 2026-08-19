const assert = require('assert');
const fs = require('fs');
const path = require('path');

const storage = new Map([
  ['apiBaseUrl', 'http://smart-score.local'],
  ['practiceRecords', [{
    id: 'record-1',
    practiceSessionId: 'session-1',
    adviceStatus: 'pending'
  }]]
]);
const modals = [];

global.wx = {
  getStorageSync(key) { return storage.get(key) || ''; },
  removeStorageSync(key) { storage.delete(key); },
  setStorageSync(key, value) { storage.set(key, value); },
  showLoading() {},
  hideLoading() {},
  showModal(options) { modals.push(options); },
  showToast() {},
  createSelectorQuery() {
    return {
      in() { return this; },
      select() { return this; },
      boundingClientRect() { return this; },
      exec() {}
    };
  }
};

global.getApp = () => ({ globalData: { currentScore: null } });

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((promiseResolve, promiseReject) => {
    resolve = promiseResolve;
    reject = promiseReject;
  });
  return { promise, resolve, reject };
}

function loadPage() {
  let definition;
  global.Page = (value) => { definition = value; };
  const pagePath = path.resolve(__dirname, '../pages/practice/practice.js');
  delete require.cache[require.resolve(pagePath)];
  require(pagePath);
  const page = {
    data: JSON.parse(JSON.stringify(definition.data || {})),
    setData(patch, callback) {
      Object.assign(this.data, patch);
      if (callback) callback();
    }
  };
  Object.keys(definition).forEach((key) => {
    if (key !== 'data') {
      page[key] = typeof definition[key] === 'function'
        ? definition[key].bind(page)
        : definition[key];
    }
  });
  return page;
}

function adviceResult() {
  return {
    summary: '保持稳定节拍。',
    focus: [{
      problem: '节奏不稳',
      evidence: ['抢拍2次'],
      practice: {
        action: '慢速练习',
        bpm: 60,
        minutes: 5,
        repetitions: 3,
        target: '连续两遍无抢拍'
      }
    }],
    next_session: {
      total_minutes: 5,
      steps: [{ action: '慢速完整演奏', minutes: 5 }]
    }
  };
}

async function run() {
  const api = require('../utils/api');
  const statusRequest = deferred();
  let statusCalls = 0;
  api.getStatus = () => {
    statusCalls += 1;
    return statusRequest.promise;
  };
  api.getCreatorStatus = () => Promise.resolve({ active: false });
  api.getPracticePreparation = () => Promise.resolve({ valid: false });

  const pollingPage = loadPage();
  const firstRefresh = pollingPage.refreshLiveStatus();
  const overlappingRefresh = pollingPage.refreshLiveStatus();
  assert.strictEqual(statusCalls, 1);
  assert.strictEqual(await overlappingRefresh, false);
  statusRequest.resolve({
    practice_state: 'READY',
    practice_session_id: 'session-1',
    advice_state: 'running',
    advice_message: '建议正在后台生成，请稍等'
  });
  await firstRefresh;
  assert.strictEqual(pollingPage.statusRefreshInFlight, false);
  assert.strictEqual(pollingPage.data.adviceState, 'running');

  let adviceCalls = 0;
  api.getPracticeAdvice = () => {
    adviceCalls += 1;
    return Promise.resolve({
      ok: true,
      session_id: 'session-1',
      state: 'ready',
      ready: true,
      advice: adviceResult()
    });
  };
  await pollingPage.applyAdviceSummary({
    practice_session_id: 'session-1',
    advice_state: 'ready',
    advice_message: '练习建议已生成'
  });
  await pollingPage.applyAdviceSummary({
    practice_session_id: 'session-1',
    advice_state: 'ready'
  });
  assert.strictEqual(adviceCalls, 1);
  assert.strictEqual(storage.get('practiceRecords')[0].adviceStatus, 'ready');
  assert.ok(storage.get('practiceRecords')[0].adviceText.includes('保持稳定节拍'));

  const runningPage = loadPage();
  runningPage.data.adviceState = 'running';
  let stopCalls = 0;
  runningPage.stopStatusPolling = () => { stopCalls += 1; };
  assert.strictEqual(runningPage.viewPracticeAdvice(), false);
  assert.strictEqual(stopCalls, 0);
  assert.strictEqual(modals[modals.length - 1].title, '建议生成中');

  const wxml = fs.readFileSync(
    path.resolve(__dirname, '../pages/practice/practice.wxml'), 'utf8'
  );
  assert.ok(wxml.includes('bindtap="viewPracticeAdvice"'));
  assert.ok(!wxml.includes('disabled="{{isAdviceLoading}}"'));

  console.log('practice socket budget tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
