const DEFAULT_TIMEOUT = 12000;
const DEFAULT_BASE_URL = 'http://smart-score.local';

function getBaseUrl() {
  return wx.getStorageSync('apiBaseUrl') || '';
}

function setBaseUrl(url) {
  const value = String(url || '').trim().replace(/\/+$/, '');
  if (!value) {
    wx.removeStorageSync('apiBaseUrl');
    return '';
  }
  wx.setStorageSync('apiBaseUrl', value);
  return value;
}

function parseResponseBody(data) {
  if (typeof data !== 'string') return data;
  try {
    return JSON.parse(data);
  } catch (err) {
    return data;
  }
}

function messageText(value, fallback = '操作失败') {
  if (value === null || value === undefined || value === '') {
    return fallback;
  }
  if (typeof value === 'string') {
    if (value === '[object Object]' || value.indexOf('Error: [object Object]') >= 0) {
      return fallback || value;
    }
    return value;
  }
  if (value instanceof Error) {
    return messageText(value.message, fallback);
  }
  if (typeof value === 'object') {
    if (value.message) return messageText(value.message, fallback);
    if (value.errMsg) return messageText(value.errMsg, fallback);
    if (value.error) return messageText(value.error, fallback);
    if (value.reason) return messageText(value.reason, fallback);
    try {
      return JSON.stringify(value).slice(0, 300);
    } catch (err) {
      return fallback;
    }
  }
  return String(value);
}

function errorMessage(err, fallback = '操作失败') {
  return messageText(err, fallback);
}

function buildRequestError(data, statusCode) {
  const parsed = parseResponseBody(data);
  if (parsed && typeof parsed === 'object') {
    const detail = messageText(parsed.message || parsed.error || parsed.reason || parsed, '');
    if (detail) {
      return new Error(`请求失败：${statusCode}，${detail}`);
    }
  }
  if (typeof parsed === 'string' && parsed.trim()) {
    return new Error(`请求失败：${statusCode}，${parsed.trim().slice(0, 120)}`);
  }
  return new Error(`请求失败：${statusCode}`);
}

function request(path, options = {}) {
  const baseUrl = options.baseUrl || getBaseUrl();
  if (!baseUrl) {
    return Promise.reject(new Error('尚未配置设备或后端地址'));
  }

  return new Promise((resolve, reject) => {
    wx.request({
      url: `${baseUrl}${path}`,
      method: options.method || 'GET',
      data: options.data || undefined,
      header: options.header || { 'content-type': 'application/json' },
      timeout: options.timeout || DEFAULT_TIMEOUT,
      success(res) {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data);
        } else {
          reject(buildRequestError(res.data, res.statusCode));
        }
      },
      fail(err) {
        reject(new Error(errorMessage(err, '网络请求失败')));
      }
    });
  });
}

function uploadFile(path, filePath, name = 'file', formData = {}) {
  const baseUrl = getBaseUrl();
  if (!baseUrl) {
    return Promise.reject(new Error('尚未配置设备或后端地址'));
  }

  return new Promise((resolve, reject) => {
    wx.uploadFile({
      url: `${baseUrl}${path}`,
      filePath,
      name,
      formData,
      success(res) {
        const data = parseResponseBody(res.data);
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(data);
        } else {
          reject(buildRequestError(data, res.statusCode));
        }
      },
      fail(err) {
        reject(new Error(errorMessage(err, '上传失败')));
      }
    });
  });
}

function readFileArrayBuffer(filePath) {
  return new Promise((resolve, reject) => {
    wx.getFileSystemManager().readFile({
      filePath,
      success: (res) => resolve(res.data),
      fail: (err) => reject(new Error(errorMessage(err, '读取文件失败')))
    });
  });
}

function uploadRawFile(path, filePath, mimeType = 'application/octet-stream') {
  const baseUrl = getBaseUrl();
  if (!baseUrl) {
    return Promise.reject(new Error('尚未配置设备或后端地址'));
  }

  return readFileArrayBuffer(filePath).then((data) => new Promise((resolve, reject) => {
    wx.request({
      url: `${baseUrl}${path}`,
      method: 'POST',
      data,
      header: { 'content-type': mimeType },
      timeout: DEFAULT_TIMEOUT * 2,
      success(res) {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data);
        } else {
          reject(buildRequestError(res.data, res.statusCode));
        }
      },
      fail(err) {
        reject(new Error(errorMessage(err, '网络请求失败')));
      }
    });
  }));
}

function uploadSheetImages(files) {
  const tasks = (files || []).map((file, index) => (
    uploadFile('/api/sheet_image', file.path || file.tempFilePath, 'sheet', {
      index: String(index),
      total: String(files.length),
      name: file.name || `sheet_${index + 1}.jpg`
    })
  ));
  return Promise.all(tasks);
}

const DEVICE_VOLUME_MAX = 80;

function uiVolumeToDevice(value) {
  const uiValue = Math.max(0, Math.min(100, Number(value) || 0));
  return Math.round(uiValue * DEVICE_VOLUME_MAX / 100);
}

function deviceVolumeToUi(value) {
  const deviceValue = Math.max(0, Math.min(DEVICE_VOLUME_MAX, Number(value) || 0));
  return Math.round(deviceValue * 100 / DEVICE_VOLUME_MAX);
}

function getAudioFiles(page = 1, search = '') {
  const safePage = Math.max(1, Number(page) || 1);
  const query = `page=${safePage}&page_size=10&search=${encodeURIComponent(String(search || '').trim())}`;
  return request(`/api/audio/files?${query}`);
}

function getSdScores(page = 1, search = '') {
  const safePage = Math.max(1, Number(page) || 1);
  const query = `page=${safePage}&page_size=20&search=${encodeURIComponent(String(search || '').trim())}`;
  return request(`/api/scores/sd?${query}`);
}

module.exports = {
  getBaseUrl,
  DEFAULT_BASE_URL,
  messageText,
  errorMessage,
  setBaseUrl,
  request,
  uploadFile,
  DEVICE_VOLUME_MAX,
  uiVolumeToDevice,
  deviceVolumeToUi,
  getAudioStatus: () => request('/api/audio/status'),
  setAudioVolume: (volume) => request('/api/audio/volume', { method: 'POST', data: { percent: uiVolumeToDevice(volume) } }),
  setAudioMute: (muted) => request('/api/audio/mute', { method: 'POST', data: { muted } }),
  playTone: (frequency, durationMs) => request('/api/audio/tone', {
    method: 'POST',
    data: { frequency_hz: frequency, duration_ms: durationMs }
  }),
  stopAudio: () => request('/api/audio/stop', { method: 'POST' }),
  startMetronome: (bpm, beatsPerMeasure, beatUnit) => request('/api/metronome/start', {
    method: 'POST',
    data: { bpm, beats_per_measure: beatsPerMeasure, beat_unit: beatUnit }
  }),
  pauseMetronome: () => request('/api/metronome/pause', { method: 'POST' }),
  stopMetronome: () => request('/api/metronome/stop', { method: 'POST' }),
  getAudioFiles,
  getSdScores,
  renameSdScore: (filename, title) => request('/api/scores/sd/rename', {
    method: 'POST',
    data: { filename, title }
  }),
  selectSdScore: (filename) => request('/api/practice/preparation/select', {
    method: 'POST',
    data: { filename }
  }),
  getPracticePreparation: () => request('/api/practice/preparation'),
  updatePracticePreparation: (options) => request('/api/practice/preparation/options', {
    method: 'POST',
    data: options
  }),
  startPreparedPractice: () => request('/api/practice/preparation/start', {
    method: 'POST'
  }),
  getSdScoreFile: (filename) => request(`/api/scores/sd/file?name=${encodeURIComponent(filename)}`),
  getCreatorStatus: () => request('/api/creator/status'),
  startCreator: (config) => request('/api/creator/start', {
    method: 'POST',
    data: config
  }),
  creatorAction: (action) => request('/api/creator/action', {
    method: 'POST',
    data: { action }
  }),
  playAudioFile: (name) => request('/api/audio/file/play', { method: 'POST', data: { name } }),
  pauseAudioFile: () => request('/api/audio/file/pause', { method: 'POST' }),
  stopAudioFile: () => request('/api/audio/file/stop', { method: 'POST' }),
  getStatus: () => request('/api/status'),
  getInputStatus: () => request('/api/input/status'),
  selectInputSource: (source) => request('/api/input/select', {
    method: 'POST',
    data: { source }
  }),
  uploadScore: (score) => request('/api/score', { method: 'POST', data: score }),
  startPractice: () => request('/api/start', { method: 'POST' }),
  forceStartPractice: () => request('/api/force_start', { method: 'POST' }),
  stopPractice: () => request('/api/stop', { method: 'POST' }),
  getResult: () => request('/api/result'),
  clearAll: () => request('/api/clear', { method: 'POST' }),
  requestAiScore: () => request('/api/ai/score', { method: 'POST' }),
  uploadSheetImage: (filePath) => uploadFile('/api/sheet_image', filePath, 'sheet'),
  uploadSheetImages,
  recognizeSheetImage: (filePath) => uploadRawFile('/api/ai/sheet_to_score', filePath, 'image/jpeg'),
  showUploadedSheet: () => request('/api/epaper/show_uploaded_sheets', { method: 'POST' })
};
