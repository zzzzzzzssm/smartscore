const api = require('../../utils/api');
const scoreLibrary = require('../../utils/score_library');
const { parseMidiToScore } = require('../../utils/util');

Page({
  data: {
    activeSource: 'upload',
    localScores: [],
    sdScores: [],
    sdSearch: '',
    sdPage: 1,
    sdTotalPages: 0,
    sdTotal: 0,
    sdPresent: true,
    sdLoading: false,
    currentScoreId: ''
  },

  onShow() {
    this.loadLocalScores();
    if (this.data.activeSource === 'sd') this.refreshSdScores(1);
  },

  loadLocalScores() {
    const current = scoreLibrary.getCurrentScore();
    this.setData({
      localScores: scoreLibrary.listScores(),
      currentScoreId: current ? current.id : ''
    });
  },

  switchSource(event) {
    const source = event.currentTarget.dataset.source;
    if (!source || source === this.data.activeSource) return;
    this.setData({ activeSource: source });
    if (source === 'sd') this.refreshSdScores(1);
    if (source === 'local') this.loadLocalScores();
  },

  chooseScoreFile() {
    wx.chooseMessageFile({
      count: 1,
      type: 'file',
      extension: ['json', 'mid', 'midi'],
      success: (res) => {
        const file = (res.tempFiles || [])[0];
        if (file) this.importScoreFile(file);
      },
      fail: (err) => {
        const message = api.errorMessage(err, '');
        if (message && message.toLowerCase().indexOf('cancel') < 0) {
          wx.showToast({ title: message, icon: 'none' });
        }
      }
    });
  },

  importScoreFile(file) {
    const name = String(file.name || '未命名乐谱');
    const lower = name.toLowerCase();
    const filePath = file.path || file.tempFilePath;
    const fs = wx.getFileSystemManager();
    wx.showLoading({ title: '正在导入' });

    if (lower.endsWith('.json')) {
      fs.readFile({
        filePath,
        encoding: 'utf8',
        success: (res) => {
          try {
            const document = JSON.parse(res.data);
            this.saveImportedScore(document, name, 'JSON 上传');
          } catch (err) {
            wx.showModal({
              title: 'JSON 解析失败',
              content: api.errorMessage(err, '文件格式不正确'),
              showCancel: false
            });
          }
        },
        fail: (err) => this.showImportError(err),
        complete: () => wx.hideLoading()
      });
      return;
    }

    if (lower.endsWith('.mid') || lower.endsWith('.midi')) {
      fs.readFile({
        filePath,
        success: (res) => {
          try {
            const document = parseMidiToScore(res.data, name);
            this.saveImportedScore(document, name, 'MIDI 上传');
          } catch (err) {
            wx.showModal({
              title: 'MIDI 解析失败',
              content: api.errorMessage(err, 'MIDI 文件格式不正确'),
              showCancel: false
            });
          }
        },
        fail: (err) => this.showImportError(err),
        complete: () => wx.hideLoading()
      });
      return;
    }

    wx.hideLoading();
    wx.showToast({ title: '请选择 JSON 或 MIDI 文件', icon: 'none' });
  },

  showImportError(err) {
    wx.showModal({
      title: '读取失败',
      content: api.errorMessage(err, '无法读取所选文件'),
      showCancel: false
    });
  },

  saveImportedScore(document, filename, source) {
    try {
      const score = scoreLibrary.saveScore(document, {
        title: document.title || filename.replace(/\.[^.]+$/, ''),
        source,
        originFilename: filename
      });
      scoreLibrary.setCurrentScore(score);
      getApp().globalData.currentScore = score;
      this.loadLocalScores();
      this.setData({ activeSource: 'local' });
      wx.showToast({ title: '已保存到手机本地', icon: 'success' });
      this.syncScore(score, false).then((synced) => {
        if (synced) wx.switchTab({ url: '/pages/practice/practice' });
      });
    } catch (err) {
      wx.showModal({
        title: '无法保存乐谱',
        content: api.errorMessage(err, '乐谱没有有效音符'),
        showCancel: false
      });
    }
  },

  syncScore(score, showSuccess = true) {
    if (!api.getBaseUrl()) {
      if (showSuccess) {
        wx.showToast({ title: '已设为当前乐谱', icon: 'success' });
      }
      return Promise.resolve(false);
    }
    return api.uploadScore(score)
      .then(() => {
        if (showSuccess) wx.showToast({ title: '已选择并同步', icon: 'success' });
        return true;
      })
      .catch((err) => {
        wx.showModal({
          title: '乐谱已选择，同步失败',
          content: api.errorMessage(err, '请检查设备连接'),
          showCancel: false
        });
        return false;
      });
  },

  selectLocalScore(event) {
    const score = this.data.localScores.find(
      (item) => item.id === event.currentTarget.dataset.id
    );
    if (!score) return;
    scoreLibrary.setCurrentScore(score);
    getApp().globalData.currentScore = score;
    this.setData({ currentScoreId: score.id });
    wx.showLoading({ title: '正在同步' });
    this.syncScore(score)
      .then((synced) => {
        if (synced) wx.switchTab({ url: '/pages/practice/practice' });
      })
      .finally(() => wx.hideLoading());
  },

  viewLocalScore(event) {
    const score = this.data.localScores.find(
      (item) => item.id === event.currentTarget.dataset.id
    );
    if (!score) return;
    wx.showModal({
      title: score.title,
      content: `${score.bpm} BPM · ${score.noteCount} 个音符\n${score.time_signature} · ${score.key}\n来源：${score.source}`,
      confirmText: '设为当前',
      success: (res) => {
        if (res.confirm) {
          this.selectLocalScore({ currentTarget: { dataset: { id: score.id } } });
        }
      }
    });
  },

  renameLocalScore(event) {
    const id = event.currentTarget.dataset.id;
    const score = this.data.localScores.find((item) => item.id === id);
    if (!score) return;
    wx.showModal({
      title: '重命名乐谱',
      editable: true,
      content: score.title,
      placeholderText: '输入乐谱名称',
      success: (res) => {
        if (!res.confirm) return;
        try {
          const renamed = scoreLibrary.renameScore(id, res.content);
          if (getApp().globalData.currentScore &&
              getApp().globalData.currentScore.id === id) {
            getApp().globalData.currentScore = renamed;
          }
          this.loadLocalScores();
          wx.showToast({ title: '已重命名', icon: 'success' });
        } catch (err) {
          wx.showToast({ title: api.errorMessage(err, '重命名失败'), icon: 'none' });
        }
      }
    });
  },

  deleteLocalScore(event) {
    const id = event.currentTarget.dataset.id;
    wx.showModal({
      title: '删除本地乐谱',
      content: '将同时删除小程序本地保存的 JSON 文件，是否继续？',
      confirmText: '删除',
      confirmColor: '#dc2626',
      success: (res) => {
        if (!res.confirm) return;
        scoreLibrary.deleteScore(id);
        if (getApp().globalData.currentScore &&
            getApp().globalData.currentScore.id === id) {
          getApp().globalData.currentScore = scoreLibrary.getCurrentScore();
        }
        this.loadLocalScores();
        wx.showToast({ title: '已删除', icon: 'success' });
      }
    });
  },

  onSdSearchInput(event) {
    this.setData({ sdSearch: event.detail.value });
  },

  searchSdScores() {
    this.refreshSdScores(1);
  },

  refreshSdScores(page = this.data.sdPage) {
    if (!api.getBaseUrl()) {
      this.setData({
        sdScores: [],
        sdPresent: false,
        sdTotal: 0,
        sdTotalPages: 0
      });
      wx.showToast({ title: '请先连接设备', icon: 'none' });
      return;
    }
    this.setData({ sdLoading: true });
    api.getSdScores(page, this.data.sdSearch)
      .then((result) => {
        this.setData({
          sdScores: Array.isArray(result.scores) ? result.scores : [],
          sdPresent: result.sd_present !== false,
          sdPage: Number(result.page || 1),
          sdTotal: Number(result.total || 0),
          sdTotalPages: Number(result.total_pages || 0)
        });
      })
      .catch((err) => wx.showModal({
        title: '无法读取设备 SD 卡',
        content: api.errorMessage(err, '请检查设备连接和 SD 卡'),
        showCancel: false
      }))
      .finally(() => this.setData({ sdLoading: false }));
  },

  previousSdPage() {
    if (this.data.sdPage > 1) this.refreshSdScores(this.data.sdPage - 1);
  },

  nextSdPage() {
    if (this.data.sdPage < this.data.sdTotalPages) {
      this.refreshSdScores(this.data.sdPage + 1);
    }
  },

  buildSdScore(filename, document) {
    return scoreLibrary.normalizeScore(document, {
      id: `sd:${filename}`,
      source: '设备 SD 卡',
      originFilename: filename
    });
  },

  selectSdScore(event) {
    const filename = event.currentTarget.dataset.filename;
    if (!filename) return;
    wx.showLoading({ title: '正在选择' });
    api.selectSdScore(filename)
      .then(() => api.getSdScoreFile(filename))
      .then((document) => {
        const score = this.buildSdScore(filename, document);
        if (!score.notes.length) throw new Error('SD 乐谱没有有效音符');
        scoreLibrary.setCurrentScore(score, true);
        getApp().globalData.currentScore = score;
        this.setData({ currentScoreId: score.id });
        wx.showToast({ title: '已选择 SD 乐谱', icon: 'success' });
        wx.switchTab({ url: '/pages/practice/practice' });
      })
      .catch((err) => wx.showModal({
        title: '选择失败',
        content: api.errorMessage(err, '无法读取这份 SD 乐谱'),
        showCancel: false
      }))
      .finally(() => wx.hideLoading());
  },

  renameSdScore(event) {
    const filename = event.currentTarget.dataset.filename;
    const currentTitle = event.currentTarget.dataset.title || '';
    if (!filename) return;

    wx.showModal({
      title: '重命名设备乐谱',
      editable: true,
      content: currentTitle,
      placeholderText: '输入新的乐谱名称',
      success: (res) => {
        if (!res.confirm) return;
        const title = String(res.content || '').trim();
        if (!title) {
          wx.showToast({ title: '乐谱名称不能为空', icon: 'none' });
          return;
        }

        wx.showLoading({ title: '正在重命名' });
        api.renameSdScore(filename, title)
          .then(() => {
            const current = getApp().globalData.currentScore;
            if (current && current.originFilename === filename) {
              current.title = title;
              getApp().globalData.currentScore = current;
            }
            wx.showToast({ title: '已重命名', icon: 'success' });
            this.refreshSdScores(this.data.sdPage);
          })
          .catch((err) => wx.showModal({
            title: '重命名失败',
            content: api.errorMessage(err, '无法修改这份 SD 乐谱'),
            showCancel: false
          }))
          .finally(() => wx.hideLoading());
      }
    });
  },

  saveSdScoreToLocal(event) {
    const filename = event.currentTarget.dataset.filename;
    if (!filename) return;
    wx.showLoading({ title: '正在保存' });
    api.getSdScoreFile(filename)
      .then((document) => {
        const score = scoreLibrary.saveScore(document, {
          source: '设备 SD 卡',
          originFilename: filename
        });
        this.loadLocalScores();
        wx.showToast({ title: '已保存到手机本地', icon: 'success' });
        return score;
      })
      .catch((err) => wx.showModal({
        title: '保存失败',
        content: api.errorMessage(err, '无法保存这份 SD 乐谱'),
        showCancel: false
      }))
      .finally(() => wx.hideLoading());
  }
});
