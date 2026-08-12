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
      content: '谱伴是一套为钢琴与键盘练习者设计的智能练习系统。\n\n你可以导入图片、JSON 或 MIDI 乐谱，连接谱伴设备后使用 USB MIDI 或麦克风跟谱练习，并获得音准、节奏和完整度评分。系统还提供练习记录、节拍器、校准音和 SD 音乐播放功能。',
      confirmText: '知道了',
      showCancel: false
    });
  },

  showGuide() {
    wx.showModal({
      title: '使用说明',
      content: '推荐使用流程：\n1. 从首页“连接设备”完成配网。\n2. 在“乐谱”中导入或选择要练习的曲谱。\n3. 进入“练习”，选择 USB MIDI 或麦克风输入后开始演奏。\n4. 结束练习后查看评分与改进建议，并在“记录”中回顾进步。\n\n未连接设备时，也可以浏览页面和调整部分练习参数。',
      confirmText: '知道了',
      showCancel: false
    });
  },

  sendFeedback() {
    wx.showModal({
      title: '联系与反馈',
      content: '遇到问题时，请记录：\n1. 出现问题的页面和操作步骤；\n2. 设备连接状态及设备 IP；\n3. 输入方式（USB MIDI 或麦克风）；\n4. 使用的乐谱或音频文件名；\n5. 异常提示，并尽量附上截图或录屏。\n\n提交建议时，请说明你的使用场景和期望效果。',
      confirmText: '知道了',
      showCancel: false
    });
  }
});
