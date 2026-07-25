const api = require('../../utils/api');

const TONES = [
  { name: 'C4', frequency: 261.63 },
  { name: 'E4', frequency: 329.63 },
  { name: 'G4', frequency: 392.00 },
  { name: 'A4', frequency: 440.00 },
  { name: 'C5', frequency: 523.25 }
];

const METERS = [
  { label: '2/4', beats: 2, unit: 4 },
  { label: '3/4', beats: 3, unit: 4 },
  { label: '4/4', beats: 4, unit: 4 },
  { label: '6/8', beats: 6, unit: 8 }
];

Page({
  data: {
    configured: false,
    online: false,
    controlOnly: false,
    statusText: '等待连接设备',
    busy: '',
    volume: 19,
    muted: false,
    playing: false,
    nowPlaying: '已停止',
    activeTone: '',
    tones: TONES,
    durationLabels: ['持续播放', '10 秒', '30 秒'],
    durations: [0, 10000, 30000],
    durationIndex: 0,
    bpm: 90,
    bpmInput: '90',
    commonBpms: [60, 80, 90, 100, 120, 140],
    meters: METERS,
    meterIndex: 2,
    beatDots: [1, 2, 3, 4],
    currentBeat: 1,
    metronomeRunning: false,
    metronomePaused: false
  },

  onShow() {
    this.stopPolling();
    this.stopBeatAnimation();
    const configured = !!api.getBaseUrl();
    this.setData({
      configured,
      online: false,
      controlOnly: false,
      statusText: configured ? '正在连接设备…' : '离线预览 · 连接设备后可播放',
      playing: false,
      nowPlaying: '已停止',
      activeTone: '',
      metronomeRunning: false,
      metronomePaused: false,
      currentBeat: 1
    });
    if (configured) {
      this.refreshStatus();
      this.statusTimer = setInterval(() => this.refreshStatus(), 1000);
    }
  },

  onHide() {
    this.stopPolling();
    this.stopBeatAnimation();
  },

  onUnload() {
    this.stopPolling();
    this.stopBeatAnimation();
  },

  onPullDownRefresh() {
    if (!this.data.configured) {
      this.showConnectPrompt();
      wx.stopPullDownRefresh();
      return;
    }
    this.refreshStatus().finally(() => wx.stopPullDownRefresh());
  },

  stopPolling() {
    if (this.statusTimer) clearInterval(this.statusTimer);
    this.statusTimer = null;
  },

  openDevicePage() {
    wx.navigateTo({ url: '/pages/device/device' });
  },

  showConnectPrompt() {
    wx.showToast({ title: '请先连接设备', icon: 'none' });
  },

  stateLabel(state) {
    const labels = { TONE: '校准音', METRONOME: '节拍器' };
    return labels[state] || '已停止';
  },

  audioErrorText(error) {
    const labels = {
      ESP_ERR_INVALID_STATE: '音频硬件尚未就绪',
      ESP_ERR_TIMEOUT: '命令队列繁忙，请稍后重试'
    };
    return labels[error] || (error && error !== 'ESP_OK' ? error : '');
  },

  refreshStatus() {
    if (!this.data.configured || this.statusRefreshing) return Promise.resolve(false);
    this.statusRefreshing = true;
    return api.getAudioStatus().then((result) => {
      const audio = result.audio || result;
      const hardware = audio.hardware || {};
      const controlOnly = !!audio.control_only ||
        (audio.ready !== false && hardware.output_enabled === false);
      const state = String(audio.state || 'STOPPED').toUpperCase();
      const playing = state === 'TONE' || state === 'METRONOME';
      const metronome = audio.metronome || {};
      const metronomeActive = !!metronome.running || !!metronome.paused;
      const audioError = this.audioErrorText(audio.last_error);
      const meterIndex = METERS.findIndex((item) => (
        item.beats === Number(metronome.beats_per_measure) &&
        item.unit === Number(metronome.beat_unit)
      ));
      const selectedMeter = metronomeActive && meterIndex >= 0
        ? meterIndex
        : this.data.meterIndex;
      const bpm = metronomeActive
        ? Number(metronome.bpm || this.data.bpm)
        : this.data.bpm;
      const nextData = {
        online: true,
        controlOnly,
        statusText: state === 'ERROR' && audioError
          ? `音频错误：${audioError}`
          : (controlOnly ? '设备在线 · 无声同步测试模式' : '设备在线 · 单声道扬声器'),
        volume: api.deviceVolumeToUi(hardware.volume_percent === undefined ? audio.volume : hardware.volume_percent),
        muted: hardware.muted === undefined ? !!audio.muted : !!hardware.muted,
        playing,
        nowPlaying: this.stateLabel(state),
        activeTone: state === 'TONE' ? this.findToneName(audio.frequency_hz) : '',
        metronomeRunning: !!metronome.running,
        metronomePaused: !!metronome.paused,
        bpm,
        meterIndex: selectedMeter,
        beatDots: Array.from({ length: METERS[selectedMeter].beats }, (_, index) => index + 1),
        currentBeat: metronomeActive
          ? Number(metronome.beat_index || this.data.currentBeat)
          : this.data.currentBeat
      };
      if (!this.bpmEditing) nextData.bpmInput = String(bpm);
      if (this.volumeChanging) delete nextData.volume;
      this.setData(nextData);
      this.syncBeatAnimation();
      return true;
    }).catch((err) => {
      this.stopBeatAnimation();
      this.setData({
        online: false,
        controlOnly: false,
        statusText: api.errorMessage(err, '设备连接失败'),
        playing: false,
        nowPlaying: '已停止',
        activeTone: '',
        metronomeRunning: false,
        metronomePaused: false,
        currentBeat: 1
      });
      return false;
    }).finally(() => {
      this.statusRefreshing = false;
    });
  },

  findToneName(frequency) {
    const value = Number(frequency);
    const tone = TONES.find((item) => Math.abs(item.frequency - value) < 0.2);
    return tone ? tone.name : '';
  },

  runCommand(name, action) {
    if (this.data.busy) return Promise.resolve(false);
    if (!this.data.online) {
      this.showConnectPrompt();
      return Promise.resolve(false);
    }
    this.setData({ busy: name });
    return action().then(() => true).catch((err) => {
      wx.showToast({ title: api.errorMessage(err), icon: 'none' });
      return false;
    }).finally(() => this.setData({ busy: '' }));
  },

  onVolumeChanging(event) {
    this.volumeChanging = true;
    this.setData({ volume: Number(event.detail.value) });
  },

  onVolumeChange(event) {
    this.volumeChanging = false;
    const volume = Number(event.detail.value);
    this.setData({ volume });
    this.runCommand('volume', () => api.setAudioVolume(volume));
  },

  toggleMute() {
    const muted = !this.data.muted;
    this.runCommand('mute', () => api.setAudioMute(muted)).then((ok) => {
      if (ok) this.setData({ muted });
    });
  },

  stopAll() {
    this.runCommand('stop', () => api.stopAudio()).then((ok) => {
      if (!ok) return;
      this.stopBeatAnimation();
      this.setData({
        playing: false,
        activeTone: '',
        metronomeRunning: false,
        metronomePaused: false,
        nowPlaying: '已停止'
      });
    });
  },

  onDurationChange(event) {
    this.setData({ durationIndex: Number(event.detail.value) });
  },

  playTone(event) {
    const name = event.currentTarget.dataset.name;
    const frequency = Number(event.currentTarget.dataset.frequency);
    const duration = this.data.durations[this.data.durationIndex];
    this.runCommand('tone', () => api.playTone(frequency, duration)).then((ok) => {
      if (!ok) return;
      this.stopBeatAnimation();
      this.setData({
        activeTone: name,
        playing: true,
        nowPlaying: `${name} · ${frequency} Hz`,
        metronomeRunning: false,
        metronomePaused: false
      });
    });
  },

  selectCommonBpm(event) {
    this.applyBpmValue(Number(event.currentTarget.dataset.bpm));
  },

  onBpmInput(event) {
    this.setData({ bpmInput: String(event.detail.value || '') });
  },

  onBpmFocus() {
    this.bpmEditing = true;
  },

  onBpmBlur() {
    this.bpmEditing = false;
  },

  applyManualBpm() {
    const text = String(this.data.bpmInput || '').trim();
    const bpm = Number(text);
    if (!/^\d{2,3}$/.test(text) || !Number.isInteger(bpm) || bpm < 30 || bpm > 240) {
      wx.showToast({ title: '请输入 30～240 的整数 BPM', icon: 'none' });
      return;
    }
    this.applyBpmValue(bpm);
  },

  applyBpmValue(bpm) {
    this.setData({ bpm, bpmInput: String(bpm) });
    if (this.data.metronomeRunning) {
      this.stopBeatAnimation();
      this.startMetronome();
    }
  },

  selectMeter(event) {
    const meterIndex = Number(event.currentTarget.dataset.index);
    const beats = METERS[meterIndex].beats;
    this.setData({ meterIndex, beatDots: Array.from({ length: beats }, (_, index) => index + 1), currentBeat: 1 });
    if (this.data.metronomeRunning) {
      this.stopBeatAnimation();
      this.startMetronome();
    }
  },

  startMetronome() {
    const bpm = Number(this.data.bpm);
    if (!Number.isInteger(bpm) || bpm < 30 || bpm > 240) {
      wx.showToast({ title: 'BPM 必须为 30～240', icon: 'none' });
      return Promise.resolve(false);
    }
    const meter = METERS[this.data.meterIndex];
    return this.runCommand('metronome', () => api.startMetronome(bpm, meter.beats, meter.unit)).then((ok) => {
      if (ok) {
        this.setData({
          metronomeRunning: true,
          metronomePaused: false,
          playing: true,
          activeTone: '',
          nowPlaying: `节拍器 · ${bpm} BPM`
        });
        this.syncBeatAnimation();
      }
      return ok;
    });
  },

  pauseMetronome() {
    this.runCommand('metronome', () => api.pauseMetronome()).then((ok) => {
      if (!ok) return;
      this.stopBeatAnimation();
      this.setData({ metronomeRunning: false, metronomePaused: true });
    });
  },

  stopMetronome() {
    this.runCommand('metronome', () => api.stopMetronome()).then((ok) => {
      if (!ok) return;
      this.stopBeatAnimation();
      this.setData({
        metronomeRunning: false,
        metronomePaused: false,
        playing: false,
        nowPlaying: '已停止',
        currentBeat: 1
      });
    });
  },

  syncBeatAnimation() {
    if (!this.data.metronomeRunning || this.data.metronomePaused) return;
    if (this.beatTimer && this.beatTimerBpm === this.data.bpm && this.beatTimerMeter === this.data.meterIndex) return;
    this.stopBeatAnimation();
    this.beatTimerBpm = this.data.bpm;
    this.beatTimerMeter = this.data.meterIndex;
    this.beatTimer = setInterval(() => {
      const beats = METERS[this.data.meterIndex].beats;
      this.setData({ currentBeat: this.data.currentBeat % beats + 1 });
    }, Math.round(60000 / this.data.bpm));
  },

  stopBeatAnimation() {
    if (this.beatTimer) clearInterval(this.beatTimer);
    this.beatTimer = null;
  }
});
