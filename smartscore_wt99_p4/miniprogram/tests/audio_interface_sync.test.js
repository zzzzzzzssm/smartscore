const assert = require('assert');
const path = require('path');

const requests = [];

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
    options.success({ statusCode: 200, data: { ok: true } });
  },
  showToast() {},
  navigateTo() {},
  stopPullDownRefresh() {}
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

  assert.strictEqual(api.uiVolumeToDevice(0), 0);
  assert.strictEqual(api.uiVolumeToDevice(100), 80);
  assert.strictEqual(api.deviceVolumeToUi(40), 50);

  await api.setAudioVolume(50);
  await api.setAudioMute(true);
  await api.playTone(440, 10000);
  await api.stopAudio();
  await api.startMetronome(120, 3, 4);
  await api.pauseMetronome();
  await api.stopMetronome();
  await api.playAudioFile('demo.wav');
  await api.pauseAudioFile();
  await api.stopAudioFile();

  assert.deepStrictEqual(
    requests.map((item) => item.path),
    [
      '/api/audio/volume',
      '/api/audio/mute',
      '/api/audio/tone',
      '/api/audio/stop',
      '/api/metronome/start',
      '/api/metronome/pause',
      '/api/metronome/stop',
      '/api/audio/file/play',
      '/api/audio/file/pause',
      '/api/audio/file/stop'
    ],
    '小程序音频操作应调用设备端约定接口'
  );
  assert.deepStrictEqual(requests[0].data, { percent: 40 });
  assert.deepStrictEqual(requests[2].data, { frequency_hz: 440, duration_ms: 10000 });
  assert.deepStrictEqual(requests[4].data, {
    bpm: 120,
    beats_per_measure: 3,
    beat_unit: 4
  });

  api.getAudioStatus = () => Promise.resolve({
    audio: {
      ready: true,
      control_only: true,
      state: 'METRONOME',
      volume: 40,
      muted: true,
      frequency_hz: 440,
      hardware: {
        output_enabled: false,
        volume_percent: 40,
        muted: true
      },
      metronome: {
        running: true,
        paused: false,
        bpm: 120,
        beats_per_measure: 3,
        beat_unit: 4,
        beat_index: 2
      }
    }
  });

  const audioPage = loadPage('../pages/audio/audio.js');
  audioPage.data.configured = true;
  await audioPage.refreshStatus();
  assert.strictEqual(audioPage.data.controlOnly, true);
  assert.strictEqual(audioPage.data.volume, 50);
  assert.strictEqual(audioPage.data.muted, true);
  assert.strictEqual(audioPage.data.metronomeRunning, true);
  assert.strictEqual(audioPage.data.bpm, 120);
  assert.strictEqual(audioPage.data.meterIndex, 1);
  assert.strictEqual(audioPage.data.currentBeat, 2);
  audioPage.stopBeatAnimation();

  api.getAudioStatus = () => Promise.resolve({
    audio: {
      ready: true,
      control_only: true,
      state: 'FILE_PAUSED',
      hardware: {
        output_enabled: false,
        volume_percent: 24,
        muted: false
      },
      file: { name: 'demo.wav' }
    }
  });
  api.getAudioFiles = () => Promise.resolve({
    sd_present: true,
    files: [{ name: 'demo.wav' }],
    page: 1,
    total: 1,
    total_pages: 1
  });

  const musicPage = loadPage('../pages/music/music.js');
  musicPage.data.configured = true;
  await musicPage.refreshStatus();
  assert.strictEqual(musicPage.data.controlOnly, true);
  assert.strictEqual(musicPage.data.volume, 30);
  assert.strictEqual(musicPage.data.activeFile, 'demo.wav');
  assert.strictEqual(musicPage.data.filePaused, true);

  console.log('audio interface sync tests passed');
}

run().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
