const assert = require('assert');
const path = require('path');

global.wx = {
  getStorageSync() {
    return [];
  },
  removeStorageSync() {},
  setStorageSync() {}
};

function loadPage(relativePath) {
  let definition;
  global.Page = (value) => {
    definition = value;
  };
  const absolutePath = path.resolve(__dirname, relativePath);
  delete require.cache[require.resolve(absolutePath)];
  require(absolutePath);
  return definition;
}

function run() {
  const page = loadPage('../pages/practice/practice.js');
  const content = page.formatPracticeAdvice({
    v: 1,
    summary: '本次节奏问题最需要优先处理。',
    focus: [{
      rank: 1,
      dimension_id: 'rhythm',
      problem: '抢拍多于拖拍',
      evidence: ['抢拍11次，拖拍6次'],
      practice: {
        action: '跟随节拍器慢速分段练习',
        bpm: 60,
        minutes: 6,
        repetitions: 4,
        target: '连续两遍节奏错误不超过3次'
      }
    }],
    next_session: {
      total_minutes: 10,
      steps: [
        { order: 1, action: '慢练错误片段', minutes: 6 },
        { order: 2, action: '完整演奏', minutes: 4 }
      ]
    },
    encouragement: '先稳住节拍，连贯性会随之改善。',
    insufficient_data: ['缺少小节编号']
  });

  assert.ok(content.includes('本次节奏问题最需要优先处理。'));
  assert.ok(content.includes('依据：抢拍11次，拖拍6次'));
  assert.ok(content.includes('60 BPM，6分钟，重复4次'));
  assert.ok(content.includes('下次练习（10分钟）'));
  assert.ok(content.includes('数据不足：缺少小节编号'));
  assert.ok(!content.includes('总分：'));
  assert.ok(!content.includes('ai_total_score'));

  assert.throws(() => page.formatPracticeAdvice({
    summary: '缺少练习步骤',
    focus: []
  }), /不完整/);

  console.log('practice advice tests passed');
}

run();
