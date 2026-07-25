const { fixed } = require('../../utils/util');

Page({
  data: {
    user: {
      nickname: '谱伴用户'
    },
    stats: {
      total: 0,
      average: '0.0',
      best: '0.0'
    }
  },

  onShow() {
    this.updateStats();
  },

  updateStats() {
    const records = wx.getStorageSync('practiceRecords') || [];
    if (!records.length) {
      this.setData({ stats: { total: 0, average: '0.0', best: '0.0' } });
      return;
    }

    const totalScore = records.reduce((sum, item) => sum + Number(item.totalScore || 0), 0);
    const best = records.reduce((max, item) => Math.max(max, Number(item.totalScore || 0)), 0);
    this.setData({
      stats: {
        total: records.length,
        average: fixed(totalScore / records.length, 1),
        best: fixed(best, 1)
      }
    });
  },

  showAbout() {
    wx.showModal({
      title: '关于谱伴',
      content: '谱伴面向钢琴/键盘练习，提供乐谱管理、演奏评分、练习记录和智能设备连接入口。',
      showCancel: false
    });
  },

  showGuide() {
    wx.showModal({
      title: '使用说明',
      content: '1. 在乐谱页添加或选择乐谱。\n2. 在练习页开始练习并结束评分。\n3. 在记录页查看历史结果。\n4. 在设备页配置 ESP32-S3 地址并同步。',
      showCancel: false
    });
  },

  sendFeedback() {
    wx.showModal({
      title: '联系与反馈',
      content: '反馈设备问题时，请一并记录设备 IP、输入模式、乐谱文件名和复现步骤，便于快速定位。',
      confirmText: '知道了',
      showCancel: false
    });
  }
});
