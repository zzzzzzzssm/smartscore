const TARGET_RATE = 24000;
const MAX_SOURCE_BYTES = 50 * 1024 * 1024;
const MAX_DURATION_SECONDS = 15 * 60;
const SUPPORTED_EXTENSIONS = ['mp3', 'm4a', 'aac', 'flac', 'ogg', 'wav'];

function fileExtension(name) {
  const match = String(name || '').toLowerCase().match(/\.([a-z0-9]+)$/);
  return match ? match[1] : '';
}

function outputName(name) {
  const stem = String(name || '音乐')
    .replace(/\.[^.]+$/, '')
    .replace(/[\\/]/g, '_')
    .replace(/\.\./g, '_')
    .trim()
    .slice(0, 60) || '音乐';
  return `${stem}.wav`;
}

function readFile(filePath) {
  return new Promise((resolve, reject) => {
    wx.getFileSystemManager().readFile({
      filePath,
      success: (res) => resolve(res.data),
      fail: (err) => reject(new Error(err.errMsg || '读取音乐文件失败'))
    });
  });
}

function statFile(filePath) {
  return new Promise((resolve, reject) => {
    wx.getFileSystemManager().stat({
      path: filePath,
      success: (res) => resolve(res.stats),
      fail: (err) => reject(new Error(err.errMsg || '读取音乐文件信息失败'))
    });
  });
}

function isCompatibleWav(buffer) {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < 44) return false;
  const bytes = new Uint8Array(buffer);
  const text = (offset, length) => String.fromCharCode(...bytes.slice(offset, offset + length));
  if (text(0, 4) !== 'RIFF' || text(8, 4) !== 'WAVE') return false;
  const view = new DataView(buffer);
  let offset = 12;
  let formatOk = false;
  let dataFound = false;
  while (offset + 8 <= buffer.byteLength) {
    const type = text(offset, 4);
    const size = view.getUint32(offset + 4, true);
    const body = offset + 8;
    if (body + size > buffer.byteLength) return false;
    if (type === 'fmt ' && size >= 16) {
      const format = view.getUint16(body, true);
      const channels = view.getUint16(body + 2, true);
      const sampleRate = view.getUint32(body + 4, true);
      const bits = view.getUint16(body + 14, true);
      formatOk = format === 1 && channels === 1 && bits === 16 &&
        (sampleRate === 16000 || sampleRate === 24000);
    } else if (type === 'data') {
      dataFound = size > 0;
    }
    if (formatOk && dataFound) return true;
    offset = body + size + (size % 2);
  }
  return false;
}

function decodeAudio(context, data) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const success = (buffer) => {
      if (!settled) {
        settled = true;
        resolve(buffer);
      }
    };
    const failure = () => {
      if (!settled) {
        settled = true;
        reject(new Error('当前手机不能解码该音乐格式'));
      }
    };
    try {
      const result = context.decodeAudioData(data, success, failure);
      if (result && typeof result.then === 'function') result.then(success, failure);
    } catch (err) {
      failure();
    }
  });
}

function writeAscii(view, offset, value) {
  for (let i = 0; i < value.length; i += 1) {
    view.setUint8(offset + i, value.charCodeAt(i));
  }
}

function createWavBuffer(audioBuffer, onProgress) {
  const sourceRate = Number(audioBuffer.sampleRate);
  const sourceLength = Number(audioBuffer.length);
  const channels = Number(audioBuffer.numberOfChannels);
  if (!sourceRate || !sourceLength || !channels) {
    return Promise.reject(new Error('音乐解码结果为空'));
  }
  const duration = sourceLength / sourceRate;
  if (duration > MAX_DURATION_SECONDS) {
    return Promise.reject(new Error('音乐不能超过 15 分钟'));
  }
  const targetLength = Math.max(1, Math.floor(duration * TARGET_RATE));
  const output = new ArrayBuffer(44 + targetLength * 2);
  const view = new DataView(output);
  writeAscii(view, 0, 'RIFF');
  view.setUint32(4, 36 + targetLength * 2, true);
  writeAscii(view, 8, 'WAVE');
  writeAscii(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, 1, true);
  view.setUint32(24, TARGET_RATE, true);
  view.setUint32(28, TARGET_RATE * 2, true);
  view.setUint16(32, 2, true);
  view.setUint16(34, 16, true);
  writeAscii(view, 36, 'data');
  view.setUint32(40, targetLength * 2, true);

  const channelData = [];
  for (let channel = 0; channel < channels; channel += 1) {
    channelData.push(audioBuffer.getChannelData(channel));
  }
  const ratio = sourceRate / TARGET_RATE;
  const blockFrames = 32768;
  let targetIndex = 0;
  return new Promise((resolve) => {
    const processBlock = () => {
      const limit = Math.min(targetLength, targetIndex + blockFrames);
      for (; targetIndex < limit; targetIndex += 1) {
        const position = targetIndex * ratio;
        const left = Math.min(sourceLength - 1, Math.floor(position));
        const right = Math.min(sourceLength - 1, left + 1);
        const fraction = position - left;
        let sample = 0;
        for (let channel = 0; channel < channels; channel += 1) {
          const data = channelData[channel];
          sample += data[left] + (data[right] - data[left]) * fraction;
        }
        sample = Math.max(-1, Math.min(1, sample / channels));
        view.setInt16(44 + targetIndex * 2,
          sample < 0 ? Math.round(sample * 32768) : Math.round(sample * 32767), true);
      }
      if (typeof onProgress === 'function') {
        onProgress(Math.min(99, Math.round(targetIndex * 100 / targetLength)));
      }
      if (targetIndex < targetLength) {
        setTimeout(processBlock, 0);
      } else {
        resolve(output);
      }
    };
    processBlock();
  });
}

function writeOutput(buffer) {
  const path = `${wx.env.USER_DATA_PATH}/music_import_${Date.now()}.wav`;
  return new Promise((resolve, reject) => {
    wx.getFileSystemManager().writeFile({
      filePath: path,
      data: buffer,
      success: () => resolve(path),
      fail: (err) => reject(new Error(err.errMsg || '保存转码文件失败'))
    });
  });
}

function transcode(file, onProgress) {
  const name = String(file && file.name || '音乐');
  const path = String(file && (file.path || file.tempFilePath) || '');
  const extension = fileExtension(name);
  if (!path || !SUPPORTED_EXTENSIONS.includes(extension)) {
    return Promise.reject(new Error('请选择 MP3、M4A、AAC、FLAC、OGG 或 WAV 文件'));
  }
  return statFile(path).then((stats) => {
    if (Number(stats.size || 0) > MAX_SOURCE_BYTES) {
      throw new Error('源音乐文件不能超过 50MB');
    }
    if (typeof onProgress === 'function') onProgress(1);
    return readFile(path);
  }).then((data) => {
    if (extension === 'wav' && isCompatibleWav(data)) {
      if (typeof onProgress === 'function') onProgress(100);
      return { path, name: outputName(name), temporary: false };
    }
    if (typeof wx.createWebAudioContext !== 'function') {
      throw new Error('当前微信版本不支持手机端音乐转码');
    }
    const context = wx.createWebAudioContext();
    return decodeAudio(context, data).then((decoded) => (
      createWavBuffer(decoded, onProgress)
    )).then((wav) => writeOutput(wav)).then((outputPath) => {
      if (context && typeof context.close === 'function') context.close();
      if (typeof onProgress === 'function') onProgress(100);
      return { path: outputPath, name: outputName(name), temporary: true };
    }, (err) => {
      if (context && typeof context.close === 'function') context.close();
      throw err;
    });
  });
}

function removeTemporary(result) {
  if (!result || !result.temporary || !result.path) return;
  try {
    wx.getFileSystemManager().unlinkSync(result.path);
  } catch (err) {
    // The mini program sandbox may already have reclaimed the temporary file.
  }
}

module.exports = {
  MAX_SOURCE_BYTES,
  MAX_DURATION_SECONDS,
  SUPPORTED_EXTENSIONS,
  outputName,
  isCompatibleWav,
  transcode,
  removeTemporary
};
