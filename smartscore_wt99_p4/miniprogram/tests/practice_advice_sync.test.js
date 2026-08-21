const assert = require('assert');
const path = require('path');
const api = require('../utils/api');

const storage = new Map([
  ['apiBaseUrl', 'http://smart-score.local'],
  ['practiceRecords', []]
]);

global.wx = {
  getStorageSync(key) { return storage.get(key) || ''; },
  setStorageSync(key, value) { storage.set(key, value); },
  removeStorageSync(key) { storage.delete(key); },
  showModal() {},
  showToast() {},
  showLoading() {},
  hideLoading() {},
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

async function run() {
  const page = loadPage();
  page.data.scoreTitle = '测试曲目';
  page.data.performanceTargetNotes = [{ midi: 60, start: 0, duration: 0.5 }];
  page.refreshLiveStatus = () => Promise.resolve(false);

  const normalized = page.normalizeResult({
    ok: true,
    practice_session_id: 'session-a',
    total_score: 88,
    pitch_score: 90,
    rhythm_score: 86,
    fluency_score: 87,
    complete_score: 89,
    start_offset: 1.25,
    tempo_scale: 0.9,
    details: []
  });
  page.finishPractice(normalized);
  const saved = storage.get('practiceRecords')[0];
  assert.strictEqual(saved.practiceSessionId, 'session-a');
  assert.strictEqual(saved.adviceStatus, 'pending');
  assert.deepStrictEqual(saved.targetNotes, [{ midi: 60, start: 0, duration: 0.5 }]);
  assert.strictEqual(saved.startOffset, 1.25);
  assert.strictEqual(saved.tempoScale, 0.9);
  page.data.performanceTargetNotes[0].midi = 72;
  assert.strictEqual(saved.targetNotes[0].midi, 60);

  const missingAlignment = page.normalizeResult({ ok: true, details: [] });
  assert.strictEqual(missingAlignment.startOffset, null);
  assert.strictEqual(missingAlignment.tempoScale, null);

  await page.applyAdviceSummary({
    practice_session_id: 'session-a',
    advice_state: 'skipped_offline',
    advice_message: '本次练习结束时设备未联网，未生成建议'
  });
  assert.strictEqual(storage.get('practiceRecords')[0].adviceStatus, 'skipped_offline');

  let adviceFetchCount = 0;
  api.getPracticeAdvice = () => {
    adviceFetchCount += 1;
    return Promise.resolve({
      state: 'ready',
      session_id: 'session-ready',
      advice: {
        summary: '本次节奏整体稳定。',
        focus: [{
          problem: '右手个别音偏早',
          evidence: ['第 4 小节提前 0.12 秒'],
          practice: {
            action: '慢速分手练习',
            bpm: 60,
            minutes: 5,
            repetitions: 3,
            target: '连续三次节奏稳定'
          }
        }],
        next_session: {
          total_minutes: 10,
          steps: [{ action: '慢速热身', minutes: 10 }]
        },
        encouragement: '继续保持。',
        insufficient_data: []
      }
    });
  };
  storage.set('practiceRecords', [{
    id: 'record-ready',
    practiceSessionId: 'session-ready',
    adviceStatus: 'pending'
  }]);
  await page.applyAdviceSummary({
    practice_session_id: 'session-ready',
    advice_state: 'ready',
    advice_message: '练习建议已生成'
  });
  const readyRecord = storage.get('practiceRecords')[0];
  assert.strictEqual(readyRecord.adviceStatus, 'ready');
  assert.ok(readyRecord.adviceText.includes('本次节奏整体稳定'));
  await page.applyAdviceSummary({
    practice_session_id: 'session-ready',
    advice_state: 'ready'
  });
  assert.strictEqual(adviceFetchCount, 1);

  storage.set('practiceRecords', [{
    id: 'record-old',
    practiceSessionId: 'session-old',
    adviceStatus: 'pending'
  }]);
  await page.applyAdviceSummary({
    practice_session_id: 'session-new',
    advice_state: 'running'
  });
  assert.strictEqual(storage.get('practiceRecords')[0].adviceStatus, 'sync_missed');

  console.log('practice advice sync tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
