const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'smartscore-library-'));
const storage = new Map();

global.wx = {
  env: { USER_DATA_PATH: root.replace(/\\/g, '/') },
  getStorageSync(key) {
    return storage.has(key) ? storage.get(key) : '';
  },
  setStorageSync(key, value) {
    storage.set(key, value);
  },
  removeStorageSync(key) {
    storage.delete(key);
  },
  getFileSystemManager() {
    return {
      accessSync: fs.accessSync,
      mkdirSync: fs.mkdirSync,
      writeFileSync: fs.writeFileSync,
      unlinkSync: fs.unlinkSync
    };
  }
};

const library = require('../utils/score_library');

try {
  assert.deepStrictEqual(
    library.normalizeNotes([
      { midi: 60, start: 0, duration: 0.5 },
      { midi: 200, start: 1, duration: 0.5 },
      { midi: 62, start: -1, duration: 0.5 }
    ]).map((note) => note.midi),
    [60]
  );

  const decorated = library.normalizeNotes([{
    midi: 64,
    start: 0,
    duration: 0.5,
    start_tick: 0,
    duration_ticks: 480,
    dots: 1,
    tie_flags: 1,
    slur_start: 7,
    gliss_stop: 9,
    notation_flags: 3,
    event_index: 12
  }])[0];
  assert.strictEqual(decorated.duration_ticks, 480);
  assert.strictEqual(decorated.dots, 1);
  assert.strictEqual(decorated.tie_flags, 1);
  assert.strictEqual(decorated.slur_start, 7);
  assert.strictEqual(decorated.gliss_stop, 9);
  assert.strictEqual(decorated.notation_flags, 3);
  assert.strictEqual(decorated.event_index, 12);

  const saved = library.saveScore({
    title: '测试乐谱',
    bpm: 96,
    notes: [{ midi: 60, start: 0, duration: 0.5 }]
  }, { source: 'JSON 上传' });
  assert.strictEqual(library.listScores().length, 1);
  assert.strictEqual(fs.existsSync(saved.localFilePath), true);

  library.setCurrentScore(saved);
  assert.strictEqual(library.getCurrentScore().title, '测试乐谱');

  storage.set('scores', [
    saved,
    { id: 'score_twinkle', title: '示例', source: '本地示例', notes: [] }
  ]);
  storage.set('practiceRecords', [
    { id: 'record_20260525_1', scoreId: 'score_twinkle' },
    { id: 'record_user', scoreId: saved.id }
  ]);
  library.migrateExamples();
  assert.strictEqual(library.listScores().length, 1);
  assert.strictEqual(storage.get('practiceRecords').length, 1);

  library.deleteScore(saved.id);
  assert.strictEqual(library.listScores().length, 0);
  assert.strictEqual(fs.existsSync(saved.localFilePath), false);

  console.log('score_library tests passed');
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}
