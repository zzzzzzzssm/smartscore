const assert = require('assert');
const fs = require('fs');
const path = require('path');

global.wx = {
  getStorageSync(key) {
    return key === 'apiBaseUrl' ? 'http://smart-score.local' : '';
  },
  removeStorageSync() {},
  setStorageSync() {},
  showLoading() {},
  hideLoading() {},
  showModal() {},
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
  global.Page = (value) => {
    definition = value;
  };
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
    ok: true,
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
  statusRequest.resolve({ practice_state: 'IDLE' });
  await firstRefresh;
  assert.strictEqual(pollingPage.statusRefreshInFlight, false);

  const adviceRequest = deferred();
  let adviceCalls = 0;
  api.requestAiScore = () => {
    adviceCalls += 1;
    return adviceRequest.promise;
  };
  const advicePage = loadPage();
  advicePage.pageVisible = true;
  let stopCalls = 0;
  let startCalls = 0;
  advicePage.stopStatusPolling = () => { stopCalls += 1; };
  advicePage.startStatusPolling = () => { startCalls += 1; };

  const firstAdvice = advicePage.requestAiScore();
  const duplicateAdvice = advicePage.requestAiScore();
  assert.strictEqual(adviceCalls, 1);
  assert.strictEqual(duplicateAdvice, false);
  assert.strictEqual(advicePage.data.isAdviceLoading, true);
  assert.strictEqual(stopCalls, 1);
  adviceRequest.resolve(adviceResult());
  await firstAdvice;
  assert.strictEqual(advicePage.data.isAdviceLoading, false);
  assert.strictEqual(startCalls, 1);

  const failedRequest = deferred();
  api.requestAiScore = () => failedRequest.promise;
  const failedPage = loadPage();
  failedPage.pageVisible = true;
  let failureRestarts = 0;
  failedPage.stopStatusPolling = () => {};
  failedPage.startStatusPolling = () => { failureRestarts += 1; };
  const failedAdvice = failedPage.requestAiScore();
  failedRequest.reject(new Error('network_interrupted'));
  await failedAdvice;
  assert.strictEqual(failedPage.data.isAdviceLoading, false);
  assert.strictEqual(failureRestarts, 1);

  const hiddenRequest = deferred();
  api.requestAiScore = () => hiddenRequest.promise;
  const hiddenPage = loadPage();
  hiddenPage.pageVisible = true;
  let hiddenRestarts = 0;
  hiddenPage.stopStatusPolling = () => {};
  hiddenPage.startStatusPolling = () => { hiddenRestarts += 1; };
  const hiddenAdvice = hiddenPage.requestAiScore();
  hiddenPage.onHide();
  hiddenRequest.resolve(adviceResult());
  await hiddenAdvice;
  assert.strictEqual(hiddenPage.pageVisible, false);
  assert.strictEqual(hiddenRestarts, 0);

  const wxml = fs.readFileSync(
    path.resolve(__dirname, '../pages/practice/practice.wxml'), 'utf8'
  );
  assert.ok(wxml.includes('disabled="{{isAdviceLoading}}"'));

  console.log('practice socket budget tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
