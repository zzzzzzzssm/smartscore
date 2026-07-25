function padNumber(num) {
  return num < 10 ? `0${num}` : `${num}`;
}

function formatTime(date) {
  const d = date instanceof Date ? date : new Date(date);
  const y = d.getFullYear();
  const m = padNumber(d.getMonth() + 1);
  const day = padNumber(d.getDate());
  const h = padNumber(d.getHours());
  const min = padNumber(d.getMinutes());
  return `${y}-${m}-${day} ${h}:${min}`;
}

function fixed(value, digits = 1) {
  const num = Number(value);
  if (!Number.isFinite(num)) {
    return '-';
  }
  return num.toFixed(digits);
}

function midiToName(midi) {
  const n = Number(midi);
  if (!Number.isFinite(n) || n < 0) {
    return '-';
  }
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  const rounded = Math.round(n);
  return `${names[rounded % 12]}${Math.floor(rounded / 12) - 1}`;
}

function scoreLevel(score) {
  const n = Number(score || 0);
  if (n >= 90) return '优秀';
  if (n >= 80) return '良好';
  if (n >= 70) return '可提升';
  return '需加强';
}

function getStoredArray(key, fallback) {
  const value = wx.getStorageSync(key);
  return Array.isArray(value) ? value : fallback;
}

function readVarLen(view, ref) {
  let value = 0;
  while (ref.pos < view.byteLength) {
    const b = view.getUint8(ref.pos);
    ref.pos += 1;
    value = (value << 7) | (b & 0x7f);
    if ((b & 0x80) === 0) break;
  }
  return value;
}

function readStr(view, ref, n) {
  let s = '';
  for (let i = 0; i < n; i += 1) {
    s += String.fromCharCode(view.getUint8(ref.pos));
    ref.pos += 1;
  }
  return s;
}

function parseMidiToScore(arrayBuffer, name) {
  const view = new DataView(arrayBuffer);
  const ref = { pos: 0 };
  if (readStr(view, ref, 4) !== 'MThd') {
    throw new Error('不是标准 MIDI 文件');
  }

  const headerLen = view.getUint32(ref.pos); ref.pos += 4;
  const format = view.getUint16(ref.pos); ref.pos += 2;
  const tracks = view.getUint16(ref.pos); ref.pos += 2;
  const division = view.getUint16(ref.pos); ref.pos += 2;
  ref.pos += Math.max(0, headerLen - 6);

  if (division & 0x8000) {
    throw new Error('暂不支持 SMPTE time-code MIDI');
  }

  const ppq = division || 480;
  let tempoUsPerBeat = 500000;
  const notes = [];

  for (let t = 0; t < tracks && ref.pos < view.byteLength; t += 1) {
    const chunk = readStr(view, ref, 4);
    const len = view.getUint32(ref.pos); ref.pos += 4;
    const end = Math.min(view.byteLength, ref.pos + len);
    if (chunk !== 'MTrk') {
      ref.pos = end;
      continue;
    }

    let tick = 0;
    let runningStatus = 0;
    const active = {};

    while (ref.pos < end) {
      tick += readVarLen(view, ref);
      let status = view.getUint8(ref.pos);
      if (status < 0x80) {
        if (!runningStatus) throw new Error('MIDI running status 缺失');
        status = runningStatus;
      } else {
        ref.pos += 1;
        runningStatus = status;
      }

      if (status === 0xff) {
        const type = view.getUint8(ref.pos); ref.pos += 1;
        const metaLen = readVarLen(view, ref);
        if (type === 0x51 && metaLen === 3) {
          tempoUsPerBeat =
            (view.getUint8(ref.pos) << 16) |
            (view.getUint8(ref.pos + 1) << 8) |
            view.getUint8(ref.pos + 2);
        }
        ref.pos += metaLen;
        continue;
      }

      if (status === 0xf0 || status === 0xf7) {
        const sysexLen = readVarLen(view, ref);
        ref.pos += sysexLen;
        continue;
      }

      const event = status & 0xf0;
      const dataBytes = (event === 0xc0 || event === 0xd0) ? 1 : 2;
      const d1 = view.getUint8(ref.pos); ref.pos += 1;
      const d2 = dataBytes === 2 ? view.getUint8(ref.pos) : 0;
      if (dataBytes === 2) ref.pos += 1;

      if (event === 0x90 && d2 > 0) {
        if (active[d1] === undefined) active[d1] = tick;
      } else if (event === 0x80 || (event === 0x90 && d2 === 0)) {
        if (active[d1] !== undefined) {
          const startTick = active[d1];
          delete active[d1];
          const secPerTick = (tempoUsPerBeat / 1000000) / ppq;
          notes.push({
            midi: d1,
            start: Number((startTick * secPerTick).toFixed(3)),
            duration: Number(Math.max((tick - startTick) * secPerTick, 0.05).toFixed(3))
          });
        }
      }
    }
    ref.pos = end;
  }

  notes.sort((a, b) => a.start - b.start || a.midi - b.midi);
  if (!notes.length) {
    throw new Error('MIDI 中没有解析到 note on/off 音符');
  }

  return {
    title: String(name || '').replace(/\.(mid|midi)$/i, '') || `midi_format_${format}`,
    bpm: Math.round(60000000 / tempoUsPerBeat),
    notes
  };
}

module.exports = {
  formatTime,
  fixed,
  midiToName,
  scoreLevel,
  getStoredArray,
  parseMidiToScore
};
