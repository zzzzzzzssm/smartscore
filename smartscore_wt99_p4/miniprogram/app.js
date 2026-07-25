const scoreLibrary = require('./utils/score_library');

App({
  globalData: {
    appName: '谱伴',
    subtitle: '智能乐谱与演奏评分助手',
    apiBaseUrl: '',
    currentScore: null
  },

  onLaunch() {
    this.bootstrapStorage();
  },

  bootstrapStorage() {
    scoreLibrary.migrateExamples();
    if (!Array.isArray(wx.getStorageSync('scores'))) {
      wx.setStorageSync('scores', []);
    }
    if (!Array.isArray(wx.getStorageSync('practiceRecords'))) {
      wx.setStorageSync('practiceRecords', []);
    }
    this.globalData.currentScore = scoreLibrary.getCurrentScore();
    this.globalData.apiBaseUrl = wx.getStorageSync('apiBaseUrl') || '';
  }
});
