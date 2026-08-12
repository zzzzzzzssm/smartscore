const assert = require('assert');
const path = require('path');

const modals = [];

global.wx = {
  getStorageSync() {
    return [];
  },
  showModal(options) {
    modals.push(options);
  }
};

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
    setData(patch) {
      Object.assign(this.data, patch);
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

function run() {
  const page = loadPage('../pages/profile/profile.js');

  page.showAbout();
  page.showGuide();
  page.sendFeedback();

  assert.strictEqual(modals.length, 3);
  assert.deepStrictEqual(modals.map((item) => item.title), ['关于谱伴', '使用说明', '联系与反馈']);
  assert.ok(modals[0].content.includes('钢琴与键盘练习者'));
  assert.ok(modals[0].content.includes('图片、JSON 或 MIDI 乐谱'));
  assert.ok(modals[0].content.includes('音准、节奏和完整度评分'));
  assert.ok(modals[1].content.includes('连接设备'));
  assert.ok(modals[1].content.includes('USB MIDI 或麦克风'));
  assert.ok(modals[1].content.includes('评分与改进建议'));
  assert.ok(!modals[1].content.includes('ESP32-S3'), '使用说明不应暴露开发板型号');
  assert.ok(modals[2].content.includes('页面和操作步骤'));
  assert.ok(modals[2].content.includes('截图或录屏'));
  assert.ok(modals[2].content.includes('使用场景和期望效果'));
  modals.forEach((item) => {
    assert.strictEqual(item.confirmText, '知道了');
    assert.strictEqual(item.showCancel, false);
  });

  console.log('profile modal content tests passed');
}

run();
