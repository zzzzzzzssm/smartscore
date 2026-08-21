const SCORE_INDEX_KEY = 'scores';
const CURRENT_SCORE_KEY = 'currentScoreId';
const EXTERNAL_SCORE_KEY = 'currentExternalScore';
const MIGRATION_KEY = 'scoreExamplesRemovedV1';
const EXAMPLE_SCORE_IDS = ['score_twinkle', 'score_ode', 'score_scale_c'];
const EXAMPLE_RECORD_IDS = ['record_20260525_1', 'record_20260524_1'];

function normalizeNotes(notes) {
  return (Array.isArray(notes) ? notes : [])
    .map((note) => {
      const normalized = {
        midi: Number(note.midi),
        start: Number(note.start),
        duration: Math.max(0.08, Number(note.duration || 0.25)),
        staff: Number(note.staff || 1),
        voice: Number(note.voice || 1)
      };
      [
        'velocity',
        'start_tick',
        'duration_ticks',
        'dots',
        'tie_flags',
        'slur_start',
        'slur_stop',
        'gliss_start',
        'gliss_stop',
        'notation_flags',
        'event_index'
      ].forEach((field) => {
        const value = Number(note[field]);
        if (Number.isFinite(value) && value >= 0) {
          normalized[field] = Math.floor(value);
        }
      });
      return normalized;
    })
    .filter((note) => (
      Number.isFinite(note.midi) &&
      note.midi >= 0 &&
      note.midi <= 127 &&
      Number.isFinite(note.start) &&
      note.start >= 0 &&
      Number.isFinite(note.duration)
    ));
}

function newScoreId() {
  return `score_${Date.now()}_${Math.floor(Math.random() * 100000)}`;
}

function todayText() {
  const date = new Date();
  const pad = (value) => String(value).padStart(2, '0');
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}`;
}

function normalizeScore(raw, overrides = {}) {
  const source = Object.assign({}, raw || {}, overrides || {});
  const notes = normalizeNotes(source.notes);
  const compactScore = source.compact_score || source.compactScore || null;
  const bpm = Number(source.bpm);
  return {
    id: String(source.id || newScoreId()),
    title: String(source.title || '未命名乐谱').trim() || '未命名乐谱',
    composer: String(source.composer || '未填写'),
    level: String(source.level || '待练习'),
    bpm: Number.isFinite(bpm) && bpm > 0 ? Math.round(bpm) : 120,
    time_signature: String(source.time_signature || '4/4'),
    key: String(source.key || 'C major'),
    noteCount: notes.length,
    updatedAt: String(source.updatedAt || todayText()),
    source: String(source.source || '手机本地'),
    originFilename: String(source.originFilename || ''),
    localFilePath: String(source.localFilePath || ''),
    compact_score: compactScore,
    eventCount: Number(source.eventCount || source.event_count || 0),
    playbackReady: source.playbackReady !== false &&
      source.playback_ready !== false,
    notes
  };
}

function getScoreDirectory() {
  return `${wx.env.USER_DATA_PATH}/scores`;
}

function ensureScoreDirectory() {
  const directory = getScoreDirectory();
  const fs = wx.getFileSystemManager();
  try {
    fs.accessSync(directory);
  } catch (err) {
    fs.mkdirSync(directory, true);
  }
  return directory;
}

function writeScoreFile(score) {
  const directory = ensureScoreDirectory();
  const filePath = `${directory}/${score.id}.json`;
  const document = {
    title: score.title,
    bpm: score.bpm,
    time_signature: score.time_signature,
    key: score.key,
    source: score.source,
    compact_score: score.compact_score,
    event_count: score.eventCount,
    playback_ready: score.playbackReady,
    notes: score.notes
  };
  wx.getFileSystemManager().writeFileSync(
    filePath,
    JSON.stringify(document),
    'utf8'
  );
  return filePath;
}

function listScores() {
  const stored = wx.getStorageSync(SCORE_INDEX_KEY);
  return Array.isArray(stored) ? stored.map((item) => normalizeScore(item, {
    id: item.id,
    updatedAt: item.updatedAt,
    source: item.source,
    originFilename: item.originFilename,
    localFilePath: item.localFilePath
  })) : [];
}

function persistIndex(scores) {
  wx.setStorageSync(SCORE_INDEX_KEY, scores);
  return scores;
}

function saveScore(raw, overrides = {}) {
  const score = normalizeScore(raw, Object.assign({}, overrides, {
    id: overrides.id || newScoreId()
  }));
  if (!score.notes.length && !score.compact_score) {
    throw new Error('乐谱没有可用音符');
  }
  score.localFilePath = writeScoreFile(score);
  const scores = listScores().filter((item) => item.id !== score.id);
  scores.unshift(score);
  persistIndex(scores);
  return score;
}

function renameScore(id, title) {
  const nextTitle = String(title || '').trim();
  if (!nextTitle) throw new Error('名称不能为空');
  const scores = listScores();
  const index = scores.findIndex((item) => item.id === id);
  if (index < 0) throw new Error('乐谱不存在');
  scores[index] = Object.assign({}, scores[index], {
    title: nextTitle,
    updatedAt: todayText()
  });
  scores[index].localFilePath = writeScoreFile(scores[index]);
  persistIndex(scores);
  return scores[index];
}

function deleteScore(id) {
  const scores = listScores();
  const score = scores.find((item) => item.id === id);
  persistIndex(scores.filter((item) => item.id !== id));
  if (score && score.localFilePath) {
    const expectedPrefix = `${getScoreDirectory()}/score_`;
    if (score.localFilePath.indexOf(expectedPrefix) === 0 &&
        score.localFilePath.endsWith('.json')) {
      try {
        wx.getFileSystemManager().unlinkSync(score.localFilePath);
      } catch (err) {
        // The index is authoritative; a previously missing file needs no action.
      }
    }
  }
  if (wx.getStorageSync(CURRENT_SCORE_KEY) === id) {
    wx.removeStorageSync(CURRENT_SCORE_KEY);
  }
}

function setCurrentScore(score, external = false) {
  if (!score) {
    wx.removeStorageSync(CURRENT_SCORE_KEY);
    wx.removeStorageSync(EXTERNAL_SCORE_KEY);
    return;
  }
  wx.setStorageSync(CURRENT_SCORE_KEY, score.id);
  if (external) {
    wx.setStorageSync(EXTERNAL_SCORE_KEY, score);
  } else {
    wx.removeStorageSync(EXTERNAL_SCORE_KEY);
  }
}

function getCurrentScore() {
  const id = wx.getStorageSync(CURRENT_SCORE_KEY);
  const local = listScores().find((item) => item.id === id);
  if (local) return local;
  const external = wx.getStorageSync(EXTERNAL_SCORE_KEY);
  return external && external.id === id ? normalizeScore(external, {
    id: external.id,
    source: external.source,
    originFilename: external.originFilename
  }) : null;
}

function migrateExamples() {
  if (wx.getStorageSync(MIGRATION_KEY)) return;
  const scoreIds = new Set(EXAMPLE_SCORE_IDS);
  const recordIds = new Set(EXAMPLE_RECORD_IDS);
  const storedScores = wx.getStorageSync(SCORE_INDEX_KEY);
  const storedRecords = wx.getStorageSync('practiceRecords');
  const scores = (Array.isArray(storedScores) ? storedScores : []).filter((item) => (
    !scoreIds.has(item.id) && item.source !== '本地示例'
  ));
  const records = (Array.isArray(storedRecords) ? storedRecords : []).filter((item) => (
    !recordIds.has(item.id) && !scoreIds.has(item.scoreId)
  ));
  persistIndex(scores);
  wx.setStorageSync('practiceRecords', records);
  if (scoreIds.has(wx.getStorageSync(CURRENT_SCORE_KEY))) {
    wx.removeStorageSync(CURRENT_SCORE_KEY);
    wx.removeStorageSync(EXTERNAL_SCORE_KEY);
  }
  wx.setStorageSync(MIGRATION_KEY, true);
}

module.exports = {
  normalizeNotes,
  normalizeScore,
  listScores,
  saveScore,
  renameScore,
  deleteScore,
  setCurrentScore,
  getCurrentScore,
  migrateExamples
};
