const assert = require('assert');
const path = require('path');
const api = require('../utils/api');
const practiceRecord = require('../utils/practice_record');

const storage = new Map([
  ['apiBaseUrl', 'http://smart-score.local'],
  ['practiceRecords', []]
]);
const toasts = [];

global.wx = {
  getStorageSync(key) { return storage.get(key) || ''; },
  setStorageSync(key, value) { storage.set(key, value); },
  removeStorageSync(key) { storage.delete(key); },
  showModal() {},
  showLoading() {},
  hideLoading() {},
  showToast(options) { toasts.push(options.title); }
};
global.getApp = () => ({ globalData: { currentScore: null } });

function loadPage(relativePath) {
  let definition;
  global.Page = (value) => { definition = value; };
  const pagePath = path.resolve(__dirname, relativePath);
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

function scoreResult(sessionId) {
  return {
    ok: true,
    practice_session_id: sessionId,
    total_score: 91,
    pitch_score: 92,
    rhythm_score: 90,
    fluency_score: 89,
    complete_score: 93,
    target_count: 1,
    details: [{
      ref_index: 1,
      target_midi: 60,
      target_start: 0,
      target_duration: 0.5,
      played_midi: 60,
      played_start: 0.1,
      played_duration: 0.5,
      result: 'matched'
    }]
  };
}

async function run() {
  let resultCalls = 0;
  api.getStatus = () => Promise.resolve({
    practice_state: 'READY',
    practice_session_id: 'session-auto',
    advice_state: 'running',
    advice_message: '建议生成中'
  });
  api.getCreatorStatus = () => Promise.resolve({ active: false });
  api.getPracticePreparation = () => Promise.resolve({
    valid: true,
    title: '自动结束测试曲'
  });
  api.getResult = () => {
    resultCalls += 1;
    return Promise.resolve(scoreResult('session-auto'));
  };

  const practicePage = loadPage('../pages/practice/practice.js');
  practicePage.data.isRecording = true;
  practicePage.data.scoreTitle = '自动结束测试曲';
  practicePage.data.performanceTargetNotes = [
    { midi: 60, start: 0, duration: 0.5 }
  ];
  await practicePage.refreshLiveStatus();
  assert.strictEqual(storage.get('practiceRecords').length, 1);
  assert.strictEqual(storage.get('practiceRecords')[0].practiceSessionId, 'session-auto');
  assert.strictEqual(practicePage.data.status, '已完成');
  assert.ok(toasts.includes('已自动保存记录'));

  await practicePage.refreshLiveStatus();
  practicePage.finishPractice(
    practicePage.normalizeResult(scoreResult('session-auto')),
    { silent: true }
  );
  assert.strictEqual(resultCalls, 1);
  assert.strictEqual(storage.get('practiceRecords').length, 1);

  storage.set('practiceRecords', []);
  practiceRecord.rememberPendingContext({
    scoreTitle: '记录页恢复测试曲',
    targetNotes: [{ midi: 60, start: 0, duration: 0.5 }]
  });
  api.getStatus = () => Promise.resolve({
    practice_state: 'READY',
    practice_session_id: 'session-history'
  });
  api.getResult = () => Promise.resolve(scoreResult('session-history'));
  const historyPage = loadPage('../pages/history/history.js');
  await historyPage.recoverLatestResult();
  assert.strictEqual(storage.get('practiceRecords').length, 1);
  assert.strictEqual(storage.get('practiceRecords')[0].practiceSessionId, 'session-history');
  assert.strictEqual(storage.get('practiceRecords')[0].scoreTitle, '记录页恢复测试曲');

  console.log('practice automatic record tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
