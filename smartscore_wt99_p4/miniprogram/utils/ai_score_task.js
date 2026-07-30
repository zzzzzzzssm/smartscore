const api = require('./api');
const scoreLibrary = require('./score_library');

let nextTaskId = 0;
let state = {
  status: 'idle',
  taskId: 0,
  imagePath: '',
  message: '',
  scoreId: '',
  noteCount: 0
};
const listeners = [];

function getState() {
  return Object.assign({}, state);
}

function notify() {
  const snapshot = getState();
  listeners.slice().forEach((listener) => {
    try {
      listener(snapshot);
    } catch (err) {
      // A page listener must not interrupt the global recognition task.
    }
  });
}

function updateState(patch) {
  state = Object.assign({}, state, patch);
  notify();
}

function subscribe(listener) {
  if (typeof listener !== 'function') return;
  if (listeners.indexOf(listener) < 0) listeners.push(listener);
  listener(getState());
}

function unsubscribe(listener) {
  const index = listeners.indexOf(listener);
  if (index >= 0) listeners.splice(index, 1);
}

function start(imagePath) {
  if (state.status === 'running') {
    wx.showToast({ title: 'AI 识谱正在后台进行', icon: 'none' });
    return false;
  }

  const taskId = ++nextTaskId;
  updateState({
    status: 'running',
    taskId,
    imagePath,
    message: 'AI 正在后台识谱，可继续使用其他功能',
    scoreId: '',
    noteCount: 0
  });
  wx.showToast({ title: '已开始后台识谱', icon: 'none' });

  api.recognizeSheetImage(imagePath)
    .then((result) => {
      if (state.taskId !== taskId) return;
      if (!result || result.ok === false ||
          !Array.isArray(result.notes) || !result.notes.length) {
        throw new Error(api.messageText(
          result && result.message,
          'AI 未返回有效音符'
        ));
      }

      const document = {
        title: result.title || 'AI 识别乐谱',
        bpm: Number(result.bpm || 120),
        time_signature: result.time_signature || '4/4',
        key: result.key || 'C',
        notes: result.notes
      };
      const score = scoreLibrary.saveScore(document, {
        title: document.title,
        source: 'AI 识谱',
        originFilename: `ai-score-${Date.now()}.json`
      });
      scoreLibrary.setCurrentScore(score);
      const app = getApp();
      if (app && app.globalData) app.globalData.currentScore = score;

      updateState({
        status: 'succeeded',
        message: `AI 识谱完成，已保存 ${score.noteCount} 个音符`,
        scoreId: score.id,
        noteCount: score.noteCount
      });
      wx.showToast({ title: 'AI 识谱完成，已保存', icon: 'success' });
    })
    .catch((err) => {
      if (state.taskId !== taskId) return;
      updateState({
        status: 'failed',
        message: api.errorMessage(err, 'AI 识谱失败'),
        scoreId: '',
        noteCount: 0
      });
      wx.showToast({
        title: 'AI 识谱失败，请返回查看',
        icon: 'none',
        duration: 2500
      });
    });

  return true;
}

module.exports = {
  getState,
  subscribe,
  unsubscribe,
  start
};
