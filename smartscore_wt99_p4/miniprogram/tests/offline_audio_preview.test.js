const assert = require('assert');
const fs = require('fs');
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

  let audioRequests = 0;
  [
    'setAudioVolume',
    'setAudioMute',
    'stopAudio',
    'playTone',
    'startMetronome',
    'pauseMetronome',
    'stopMetronome'
  ].forEach((name) => {
    api[name] = () => {
      audioRequests += 1;
      return Promise.resolve();
    };
  });
  api.getBaseUrl = () => '';

  const audioPage = loadPage('../pages/audio/audio.js');
  audioPage.onShow();
  assert.strictEqual(audioPage.data.statusText, '设备未连接', '音频页顶部应显示真实未连接状态');
  audioPage.selectCommonBpm({ currentTarget: { dataset: { bpm: 120 } } });
  audioPage.selectMeter({ currentTarget: { dataset: { index: 1 } } });
  audioPage.onDurationChange({ detail: { value: 2 } });
  const audioToastStart = toasts.length;
  audioPage.onVolumeChange({ detail: { value: 50 } });
  audioPage.toggleMute();
  audioPage.stopAll();
  audioPage.playTone({ currentTarget: { dataset: { name: 'A4', frequency: 440 } } });
  await audioPage.startMetronome();
  audioPage.pauseMetronome();
  audioPage.stopMetronome();

  assert.strictEqual(audioPage.data.bpm, 120, '未连接时应允许调整 BPM');
  assert.strictEqual(audioPage.data.meterIndex, 1, '未连接时应允许调整拍号');
  assert.strictEqual(audioPage.data.durationIndex, 2, '未连接时应允许调整校准音时长');
  assert.strictEqual(audioRequests, 0, '未连接时不应发送音频设备请求');
  assert.deepStrictEqual(
    toasts.slice(audioToastStart),
    Array(7).fill('设备未连接'),
    '节拍器与校准音页面的设备操作应统一提示设备未连接'
  );

  let musicRequests = 0;
  [
    'setAudioVolume',
    'setAudioMute',
    'getAudioFiles',
    'playAudioFile',
    'pauseAudioFile',
    'stopAudioFile'
  ].forEach((name) => {
    api[name] = () => {
      musicRequests += 1;
      return Promise.resolve({ files: [] });
    };
  });

  const musicPage = loadPage('../pages/music/music.js');
  musicPage.onShow();
  assert.strictEqual(musicPage.data.statusText, '设备未连接', '音乐页顶部应显示真实未连接状态');
  musicPage.data.searchInput = 'test';
  musicPage.data.activeFile = 'test.wav';
  const musicToastStart = toasts.length;
  musicPage.onVolumeChange({ detail: { value: 50 } });
  musicPage.toggleMute();
  musicPage.searchFiles();
  musicPage.clearSearch();
  musicPage.refreshFiles();
  musicPage.previousPage();
  musicPage.nextPage();
  musicPage.playFile({ currentTarget: { dataset: { name: 'test.wav' } } });
  musicPage.pauseFile();
  musicPage.stopFile();

  assert.strictEqual(musicRequests, 0, '未连接时不应发送音乐设备请求');
  assert.strictEqual(musicPage.data.emptyText, '没有找到可播放的 WAV 文件');
  assert.deepStrictEqual(
    toasts.slice(musicToastStart),
    Array(10).fill('设备未连接'),
    '音乐页面的设备操作应统一提示设备未连接'
  );

  const audioWxml = fs.readFileSync(path.resolve(__dirname, '../pages/audio/audio.wxml'), 'utf8');
  const musicWxml = fs.readFileSync(path.resolve(__dirname, '../pages/music/music.wxml'), 'utf8');
  [audioWxml, musicWxml].forEach((content) => {
    assert.ok(!content.includes('wx:if="{{!online}}"'), '页面不应渲染离线预览卡片');
    assert.ok(!content.includes('离线预览'), '页面不应包含离线预览文案');
    assert.ok(!content.includes('连接设备后'), '页面主体不应包含连接后使用说明');
    assert.ok(content.includes('<view class="volume-value">{{volume}}</view>'), '音量区域应显示当前数字');
    assert.ok(!content.includes('页面 0～100'), '音量区域不应显示小字说明');
  });

  api.getBaseUrl = () => {
    throw new Error('首页音频入口不应检查设备地址');
  };
  const indexPage = loadPage('../pages/index/index.js');
  indexPage.openAudioPage('/pages/audio/audio');

  assert.ok(navigations.includes('/pages/audio/audio'), '未连接时首页应允许进入音频工具页');
  console.log('offline audio UI cleanup tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
