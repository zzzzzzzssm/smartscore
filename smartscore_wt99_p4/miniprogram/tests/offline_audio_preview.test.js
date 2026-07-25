const assert = require('assert');
const path = require('path');

const toasts = [];
const navigations = [];

global.wx = {
  showToast(options) {
    toasts.push(options && options.title);
  },
  navigateTo(options) {
    navigations.push(options && options.url);
  },
  stopPullDownRefresh() {},
  getStorageSync() {
    return '';
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

async function run() {
  const api = require('../utils/api');

  let metronomeRequests = 0;
  api.startMetronome = () => {
    metronomeRequests += 1;
    return Promise.resolve();
  };

  const audioPage = loadPage('../pages/audio/audio.js');
  audioPage.data.online = false;
  audioPage.selectCommonBpm({ currentTarget: { dataset: { bpm: 120 } } });
  audioPage.onDurationChange({ detail: { value: 2 } });
  await audioPage.startMetronome();

  assert.strictEqual(audioPage.data.bpm, 120, '离线时应允许调整 BPM');
  assert.strictEqual(audioPage.data.durationIndex, 2, '离线时应允许调整校准音时长');
  assert.strictEqual(metronomeRequests, 0, '离线时不应发送节拍器请求');
  assert.ok(toasts.includes('请先连接设备'), '离线设备操作应提示先连接');

  let fileRequests = 0;
  api.getAudioFiles = () => {
    fileRequests += 1;
    return Promise.resolve({ files: [] });
  };

  const musicPage = loadPage('../pages/music/music.js');
  musicPage.data.online = false;
  musicPage.data.searchInput = 'test';
  musicPage.searchFiles();

  assert.strictEqual(fileRequests, 0, '离线搜索不应读取设备 SD 卡');
  assert.strictEqual(musicPage.data.emptyText, '连接设备后读取 SD 卡内容');

  api.getBaseUrl = () => {
    throw new Error('首页音频入口不应检查设备地址');
  };
  const indexPage = loadPage('../pages/index/index.js');
  indexPage.openAudioPage('/pages/audio/audio');

  assert.ok(navigations.includes('/pages/audio/audio'), '离线时首页应允许进入音频工具页');
  console.log('offline audio preview tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
