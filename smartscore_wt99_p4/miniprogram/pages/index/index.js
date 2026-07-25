const { scoreLevel } = require('../../utils/util');
const api = require('../../utils/api');

Page({
  data: {
    lastRecord: null
  },

  onShow() {
    const records = wx.getStorageSync('practiceRecords') || [];
    const last = records[0] || null;
    if (last) {
      last.level = scoreLevel(last.totalScore);
    }
    this.setData({ lastRecord: last });
  },

  goScores() {
    wx.switchTab({
      url: '/pages/scores/scores',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  },

  goPractice() {
    wx.switchTab({
      url: '/pages/practice/practice',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  },

  openAudioPage(url) {
    wx.navigateTo({
      url,
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  },

  goAudioTools() {
    this.openAudioPage('/pages/audio/audio');
  },

  goMusic() {
    this.openAudioPage('/pages/music/music');
  },

  goCreator() {
    wx.navigateTo({
      url: '/pages/creator/creator',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  },

  goHistory() {
    wx.switchTab({
      url: '/pages/history/history',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  },

  goDevice() {
    wx.navigateTo({
      url: '/pages/device/device',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  }
});
