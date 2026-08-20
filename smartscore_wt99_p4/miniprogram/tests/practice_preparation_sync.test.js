const assert = require('assert');
const fs = require('fs');
const path = require('path');
const performanceChart = require('../utils/performance_chart');

const requests = [];
const toasts = [];

global.wx = {
  getStorageSync(key) {
    return key === 'apiBaseUrl' ? 'http://smart-score.local' : '';
  },
  request(options) {
    requests.push({
      path: options.url.replace('http://smart-score.local', ''),
      method: options.method,
      data: options.data
    });
    options.success({
      statusCode: 200,
      data: {
        ok: true,
        preparation: {
          valid: true,
          revision: requests.length,
          phase: 'prepared',
          title: '测试乐谱',
          notation_type: 'staff',
          mode: 'follow',
          input_source: 'usb_midi'
        }
      }
    });
  },
  showLoading() {},
  hideLoading() {},
  showModal() {},
  showToast(options) {
    toasts.push(options.title);
  },
  createSelectorQuery() {
    return {
      in() { return this; },
      select() { return this; },
      boundingClientRect(callback) {
        this.callback = callback;
        return this;
      },
      exec() {
        if (this.callback) this.callback(null);
      }
    };
  }
};

global.getApp = () => ({ globalData: { currentScore: null } });

function loadPage(relativePath) {
  let definition;
  global.Page = (value) => {
    definition = value;
  };
  const absolutePath = path.resolve(__dirname, relativePath);
  delete require.cache[require.resolve(absolutePath)];
  require(absolutePath);
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
  const api = require('../utils/api');
  await api.getPracticePreparation();
  await api.selectSdScore('DEMO.JSON');
  await api.updatePracticePreparation({
    notation_type: 'numbered',
    mode: 'read_only'
  });
  await api.startPreparedPractice();
  await api.restartPreparedPractice();

  assert.deepStrictEqual(
    requests.map((item) => item.path),
    [
      '/api/practice/preparation',
      '/api/practice/preparation/select',
      '/api/practice/preparation/options',
      '/api/practice/preparation/start',
      '/api/practice/restart'
    ]
  );
  assert.deepStrictEqual(requests[1].data, { filename: 'DEMO.JSON' });
  assert.deepStrictEqual(requests[2].data, {
    notation_type: 'numbered',
    mode: 'read_only'
  });

  const page = loadPage('../pages/practice/practice.js');
  page.data.preparationValid = true;
  page.data.practiceMode = 'follow';
  page.data.notationType = 'staff';

  api.updatePracticePreparation = (patch) => Promise.resolve({
    preparation: {
      valid: true,
      revision: 7,
      phase: 'prepared',
      title: '测试乐谱',
      notation_type: patch.notation_type || page.data.notationType,
      mode: patch.mode || page.data.practiceMode,
      input_source: patch.input_source || page.data.selectedInput
    }
  });
  await page.updatePreparationOption({ notation_type: 'numbered' });
  assert.strictEqual(page.data.notationType, 'numbered');
  assert.strictEqual(page.data.preparationRevision, 7);

  api.startPreparedPractice = () => Promise.resolve({
    ok: true,
    preparation: {
      valid: true,
      revision: 8,
      phase: 'reading',
      title: '测试乐谱',
      notation_type: 'numbered',
      mode: page.data.practiceMode,
      input_source: 'usb_midi'
    }
  });
  page.refreshLiveStatus = () => Promise.resolve();
  page.previewScore();
  await new Promise((resolve) => setImmediate(resolve));
  assert.strictEqual(page.data.status, '只读看谱');
  assert.strictEqual(page.data.isRecording, false);
  assert.ok(toasts.includes('屏幕已显示乐谱'));

  page.data.currentScore = {
    notes: [{ midi: 60, start: 0, duration: 0.5 }]
  };
  page.startPractice();
  await new Promise((resolve) => setImmediate(resolve));
  assert.strictEqual(page.data.status, '正在记录');
  assert.strictEqual(page.data.isRecording, true);
  assert.deepStrictEqual(page.data.performanceTargetNotes, [
    { midi: 60, start: 0, duration: 0.5 }
  ]);
  page.data.currentScore.notes[0].midi = 72;
  assert.strictEqual(page.data.performanceTargetNotes[0].midi, 60);
  assert.ok(toasts.includes('已开始跟谱'));

  page.data.isRecording = false;
  page.data.result = { totalScore: 88, details: [] };
  api.restartPreparedPractice = () => Promise.resolve({
    ok: true,
    preparation: {
      valid: true,
      revision: 9,
      phase: 'following',
      title: '测试乐谱',
      notation_type: 'numbered',
      mode: 'follow',
      input_source: 'usb_midi'
    }
  });
  page.resetPractice();
  await new Promise((resolve) => setImmediate(resolve));
  assert.strictEqual(page.data.status, '正在记录');
  assert.strictEqual(page.data.isRecording, true);
  assert.strictEqual(page.data.result.totalScore, '-');
  assert.ok(toasts.includes('已重新开始'));

  const resultTargets = performanceChart.buildDetailTargets({
    details: [
      { ref_index: 2, target_midi: 62, target_start: 1, target_duration: 0.5 },
      { ref_index: 1, target_midi: 60, target_start: 0, target_duration: 0.5 },
      { ref_index: 1, target_midi: 60, target_start: 0, target_duration: 0.5 }
    ]
  });
  assert.deepStrictEqual(
    resultTargets.map((item) => [item.refIndex, item.midi, item.start]),
    [[1, 60, 0], [2, 62, 1]]
  );

  const wxml = fs.readFileSync(
    path.resolve(__dirname, '../pages/practice/practice.wxml'), 'utf8'
  );
  assert.ok(wxml.includes('简谱'));
  assert.ok(wxml.includes('五线谱'));
  assert.ok(wxml.includes('预览乐谱'));
  assert.ok(wxml.includes('开始演奏跟谱'));
  assert.ok(wxml.includes('重新练习'));
  assert.ok(wxml.includes('<performance-chart'));
  assert.ok(!wxml.includes('使用方式'));

  console.log('practice preparation sync tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
