const assert = require('assert');
const fs = require('fs');
const path = require('path');
const chart = require('../utils/performance_chart');

function detail(overrides) {
  return Object.assign({
    ref_index: 1,
    target_midi: 60,
    played_midi: 60,
    target_start: 0,
    played_start: 1,
    target_duration: 0.5,
    played_duration: 0.5,
    time_error: 0,
    result: 'correct'
  }, overrides || {});
}

function read(relativePath) {
  return fs.readFileSync(path.resolve(__dirname, relativePath), 'utf8');
}

function run() {
  assert.strictEqual(chart.finiteNumber(null), null);
  assert.strictEqual(chart.finiteNumber(undefined), null);
  assert.strictEqual(chart.finiteNumber(''), null);
  assert.strictEqual(chart.finiteNumber('  '), null);
  assert.strictEqual(chart.finiteNumber(Infinity), null);
  assert.strictEqual(chart.finiteNumber('1.5'), 1.5);

  assert.deepStrictEqual(chart.normalizeTargetNotes([
    { midi: 64, start: 1, duration: 0.5 },
    { midi: 60, start: 0, duration: 0 },
    { midi: 128, start: 2, duration: 0.5 },
    { midi: 62, start: '', duration: 0.5 }
  ]), [
    { midi: 60, start: 0, duration: 0.08 },
    { midi: 64, start: 1, duration: 0.5 }
  ]);

  const matched = chart.getMatchedDetails({
    details: [
      detail({ ref_index: 2, target_midi: 62, played_midi: 63, target_start: 1, played_start: 2.2 }),
      detail({ ref_index: 1 }),
      detail({ ref_index: 3, result: 'uncertain' }),
      detail({ ref_index: 4, target_midi: null }),
      detail({ ref_index: 5, played_start: '' }),
      detail({ ref_index: 6, played_midi: 128 })
    ]
  });
  assert.deepStrictEqual(matched.map((item) => item.ref_index), [1, 2]);

  const completeResult = {
    targetCount: 2,
    details: [
      detail({ ref_index: 2, target_midi: 62, target_start: 1 }),
      detail({ ref_index: 1, target_midi: 60, target_start: 0 })
    ]
  };
  const complete = chart.selectTargetNotes(completeResult, [
    { midi: 72, start: 9, duration: 1 }
  ]);
  assert.strictEqual(complete.source, 'details');
  assert.strictEqual(complete.partial, false);
  assert.deepStrictEqual(complete.notes.map((note) => note.midi), [60, 62]);

  const gapped = chart.selectTargetNotes({
    targetCount: 2,
    details: [
      detail({ ref_index: 1, target_midi: 60, target_start: 0 }),
      detail({ ref_index: 3, target_midi: 64, target_start: 2 })
    ]
  }, [{ midi: 72, start: 0, duration: 1 }]);
  assert.strictEqual(gapped.source, 'snapshot');
  assert.deepStrictEqual(gapped.notes.map((note) => note.midi), [72]);

  const missingCount = chart.selectTargetNotes({
    details: [detail({ ref_index: 1 })]
  }, [{ midi: 67, start: 0, duration: 1 }]);
  assert.strictEqual(missingCount.source, 'snapshot');

  const partialModel = chart.buildChartModel({
    targetCount: 3,
    details: [detail({ ref_index: 1 })]
  }, [], 'history');
  assert.strictEqual(partialModel.partial, true);
  assert.strictEqual(
    partialModel.chartHint,
    '这条历史记录仅保留了可展示的演奏片段。'
  );

  assert.strictEqual(chart.alignedPlayedStart({
    aligned_played_start: 0.5,
    played_start: 10
  }, {}, 8), 0.5);
  assert.strictEqual(chart.alignedPlayedStart({ played_start: 3 }, {
    startOffset: 1,
    tempoScale: 2
  }, 99), 1);
  assert.strictEqual(chart.alignedPlayedStart({ played_start: 3 }, {
    startOffset: null,
    tempoScale: null
  }, 1), 2);

  const stats = chart.calculateStats({ targetCount: 3 }, [
    detail({ played_midi: 61, time_error: -0.2 }),
    detail({ ref_index: 2, target_midi: 62, played_midi: 64, time_error: null })
  ], [{ midi: 60, start: 0, duration: 1 }]);
  assert.deepStrictEqual(stats, {
    avgPitch: '1.50 半音',
    maxPitch: '2.00 半音',
    avgRhythm: '0.20s',
    matched: '2 / 3'
  });

  const emptyHistory = chart.buildChartModel({}, [], 'history');
  assert.strictEqual(emptyHistory.chartEmpty, true);
  assert.strictEqual(emptyHistory.chartHint, '这条记录没有可展示的演奏数据。');

  // Page/component registration and replacement assertions are kept here so
  // the shared chart cannot silently drift back into two page-local versions.
  const practiceJson = JSON.parse(read('../pages/practice/practice.json'));
  const historyJson = JSON.parse(read('../pages/history/history.json'));
  const componentPath = '/components/performance-chart/performance-chart';
  assert.strictEqual(practiceJson.usingComponents['performance-chart'], componentPath);
  assert.strictEqual(historyJson.usingComponents['performance-chart'], componentPath);

  const practiceWxml = read('../pages/practice/practice.wxml');
  const historyWxml = read('../pages/history/history.wxml');
  assert.ok(practiceWxml.includes('<performance-chart'));
  assert.ok(historyWxml.includes('<performance-chart'));
  assert.ok(!historyWxml.includes('逐音符数据'));
  assert.ok(!historyWxml.includes('note-table'));
  assert.ok(historyWxml.includes('评分明细和演奏过程图谱'));
  assert.ok(historyWxml.includes('练习照片'));
  assert.ok(historyWxml.includes('photo-grid'));
  const historyWxss = read('../pages/history/history.wxss');
  assert.ok(historyWxss.includes('.detail-panel'));
  assert.ok(historyWxss.includes('overflow: auto'));
  const historyJs = read('../pages/history/history.js');
  assert.ok(!historyJs.includes('score_library'));
  assert.ok(historyJs.includes('loadPhotos(page)'));

  const componentWxml = read('../components/performance-chart/performance-chart.wxml');
  assert.ok(componentWxml.includes('标准谱'));
  assert.ok(componentWxml.includes('采集演奏'));
  assert.ok(componentWxml.includes('音高误差'));
  assert.ok(componentWxml.includes('平均节奏误差'));
  assert.ok(componentWxml.includes('id="performanceChart"'));
  assert.ok(componentWxml.includes('type="2d"'));
  assert.ok(!componentWxml.includes('canvas-id='));
  const componentWxss = read('../components/performance-chart/performance-chart.wxss');
  assert.ok(componentWxss.includes('.chart-wrap'));
  assert.ok(componentWxss.includes('overflow: hidden'));

  const componentJs = read('../components/performance-chart/performance-chart.js');
  assert.match(componentJs, /fields\(\{\s*node:\s*true,\s*size:\s*true\s*\}/);
  assert.ok(componentJs.includes("canvas.getContext('2d')"));
  assert.ok(componentJs.includes('ctx.scale(pixelRatio, pixelRatio)'));
  assert.ok(!componentJs.includes('wx.createCanvasContext'));
  assert.ok(!componentJs.includes('ctx.draw()'));

  let componentDefinition;
  global.Component = (definition) => { componentDefinition = definition; };
  global.wx = { nextTick(callback) { callback(); } };
  const componentJsPath = path.resolve(
    __dirname,
    '../components/performance-chart/performance-chart.js'
  );
  delete require.cache[require.resolve(componentJsPath)];
  require(componentJsPath);
  const component = {
    properties: {
      result: { targetCount: 2, details: [detail({ ref_index: 1 })] },
      targetNotes: [],
      context: 'history'
    },
    data: JSON.parse(JSON.stringify(componentDefinition.data)),
    chartAttached: true,
    setData(patch, callback) {
      Object.assign(this.data, patch);
      if (callback) callback();
    }
  };
  Object.keys(componentDefinition.methods).forEach((name) => {
    component[name] = componentDefinition.methods[name].bind(component);
  });
  let drawCount = 0;
  component.drawCanvas = () => { drawCount += 1; };
  componentDefinition.observers['result,targetNotes,context'].call(component);
  assert.strictEqual(drawCount, 1);
  assert.strictEqual(
    component.data.chartHint,
    '这条历史记录仅保留了可展示的演奏片段。'
  );
  component.properties.result = {};
  componentDefinition.observers['result,targetNotes,context'].call(component);
  assert.strictEqual(drawCount, 2);
  assert.strictEqual(component.data.chartEmpty, true);
  delete global.Component;
  delete global.wx;

  console.log('performance chart tests passed');
}

run();
