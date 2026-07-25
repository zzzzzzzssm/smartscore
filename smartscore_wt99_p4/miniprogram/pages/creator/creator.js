const api = require('../../utils/api');

Page({
  data: {
    bpmOptions: [60, 80, 100, 120],
    timeOptions: [
      { label: '2/4', num: 2, den: 4 },
      { label: '3/4', num: 3, den: 4 },
      { label: '4/4', num: 4, den: 4 },
      { label: '6/8', num: 6, den: 8 }
    ],
    bpm: 100,
    timeSigNum: 4,
    timeSigDen: 4,
    staffMode: 'grand',
    busy: false,
    status: {
      available: false,
      active: false,
      state: 'idle',
      stateLabel: '未开始',
      message: '连接设备后可开始创作',
      waiting_first_note: false,
      usb_midi_connected: false,
      note_count: 0,
      measure_count: 0,
      saved_title: '',
      saved_filename: ''
    }
  },

  onShow() {
    this.refreshStatus();
    this.stopPolling();
    this.statusTimer = setInterval(() => this.refreshStatus(), 1000);
  },

  onHide() {
    this.stopPolling();
  },

  onUnload() {
    this.stopPolling();
  },

  stopPolling() {
    if (this.statusTimer) {
      clearInterval(this.statusTimer);
      this.statusTimer = null;
    }
  },

  stateLabel(state, waiting) {
    if (state === 'recording') return waiting ? '等待首音' : '录制中';
    if (state === 'paused') return '已暂停';
    if (state === 'saving') return '保存中';
    if (state === 'error') return '需要处理';
    return '未开始';
  },

  applyStatus(result) {
    const state = String(result.state || 'idle').toLowerCase();
    const status = Object.assign({}, result, {
      active: !!result.active,
      state,
      stateLabel: this.stateLabel(state, !!result.waiting_first_note),
      note_count: Number(result.note_count || 0),
      measure_count: Number(result.measure_count || 0),
      message: result.message || '设备已就绪'
    });
    const next = { status };
    if (!status.active && Number(result.bpm) > 0) {
      next.bpm = Number(result.bpm);
      next.timeSigNum = Number(result.time_sig_num || 4);
      next.timeSigDen = Number(result.time_sig_den || 4);
      next.staffMode = result.staff_mode === 'single' ? 'single' : 'grand';
    }
    this.setData(next);
  },

  refreshStatus() {
    if (!api.getBaseUrl()) {
      this.setData({
        status: Object.assign({}, this.data.status, {
          available: false,
          active: false,
          state: 'idle',
          stateLabel: '未连接',
          message: '请先连接设备'
        })
      });
      return;
    }
    api.getCreatorStatus()
      .then((result) => this.applyStatus(result))
      .catch(() => {
        this.setData({
          status: Object.assign({}, this.data.status, {
            available: false,
            stateLabel: '设备离线',
            message: '无法读取创作者模式状态'
          })
        });
      });
  },

  chooseBpm(event) {
    if (this.data.status.active) return;
    this.setData({ bpm: Number(event.currentTarget.dataset.value) });
  },

  chooseTime(event) {
    if (this.data.status.active) return;
    this.setData({
      timeSigNum: Number(event.currentTarget.dataset.num),
      timeSigDen: Number(event.currentTarget.dataset.den)
    });
  },

  chooseStaff(event) {
    if (this.data.status.active) return;
    this.setData({ staffMode: event.currentTarget.dataset.value });
  },

  startCreator() {
    if (this.data.busy || this.data.status.active) return;
    if (!api.getBaseUrl()) {
      wx.showToast({ title: '请先连接设备', icon: 'none' });
      return;
    }
    this.setData({ busy: true });
    wx.showLoading({ title: '正在启动' });
    api.startCreator({
      bpm: this.data.bpm,
      time_sig_num: this.data.timeSigNum,
      time_sig_den: this.data.timeSigDen,
      staff_mode: this.data.staffMode
    })
      .then((result) => {
        this.applyStatus(result);
        wx.showToast({ title: '创作已开始', icon: 'success' });
      })
      .catch((err) => wx.showModal({
        title: '无法开始创作',
        content: `${api.errorMessage(err, '启动失败')}\n请先结束正在进行的练习，并确认屏幕已完成启动。`,
        showCancel: false
      }))
      .finally(() => {
        this.setData({ busy: false });
        wx.hideLoading();
      });
  },

  runAction(event) {
    const action = event.currentTarget.dataset.action;
    if (!action || this.data.busy) return;
    const loadingText = action === 'finish' ? '正在保存' : '正在执行';
    this.setData({ busy: true });
    wx.showLoading({ title: loadingText });
    api.creatorAction(action)
      .then((result) => {
        this.applyStatus(result);
        const messages = {
          pause: '已暂停',
          resume: '已继续',
          finish: '已保存到设备 SD 卡',
          cancel: '已退出创作'
        };
        wx.showToast({ title: messages[action] || '操作完成', icon: 'success' });
      })
      .catch((err) => wx.showModal({
        title: '操作未完成',
        content: api.errorMessage(
          err,
          action === 'finish' ? '请先暂停，并至少录制一个音符' : '当前状态不允许此操作'
        ),
        showCancel: false
      }))
      .finally(() => {
        this.setData({ busy: false });
        wx.hideLoading();
      });
  },

  openDevice() {
    wx.navigateTo({ url: '/pages/device/device' });
  },

  openSdScores() {
    wx.switchTab({ url: '/pages/scores/scores' });
  }
});
