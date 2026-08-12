const api = require('../../utils/api');
const utf8 = require('../../utils/utf8');

Page({
  data: {
    configured: false,
    online: false,
    controlOnly: false,
    statusText: '设备未连接',
    busy: '',
    volume: 19,
    muted: false,
    sdPresent: false,
    sdText: '暂无曲库信息',
    filesLoading: false,
    files: [],
    page: 1,
    total: 0,
    totalPages: 0,
    searchInput: '',
    activeSearch: '',
    emptyText: '没有找到可播放的 WAV 文件',
    activeFile: '',
    filePaused: false
  },

  onShow() {
    this.stopPolling();
    this.filesRequestId = (this.filesRequestId || 0) + 1;
    const configured = !!api.getBaseUrl();
    this.setData({
      configured,
      online: false,
      controlOnly: false,
      statusText: configured ? '正在连接设备…' : '设备未连接',
      sdPresent: false,
      sdText: '暂无曲库信息',
      files: [],
      total: 0,
      totalPages: 0,
      activeFile: '',
      filePaused: false,
      filesLoading: false,
      emptyText: '没有找到可播放的 WAV 文件'
    });
    if (configured) {
      this.refreshStatus();
      this.statusTimer = setInterval(() => this.refreshStatus(), 1000);
    }
  },

  onHide() {
    this.stopPolling();
    this.filesRequestId = (this.filesRequestId || 0) + 1;
    this.setData({ filesLoading: false });
  },

  onUnload() {
    this.stopPolling();
  },

  onPullDownRefresh() {
    if (!this.data.configured) {
      this.showConnectPrompt();
      wx.stopPullDownRefresh();
      return;
    }
    this.refreshStatus().then(() => {
      if (this.data.online && !this.data.filesLoading) {
        return this.loadFiles(this.data.page);
      }
      return null;
    }).finally(() => wx.stopPullDownRefresh());
  },

  stopPolling() {
    if (this.statusTimer) clearInterval(this.statusTimer);
    this.statusTimer = null;
  },

  showConnectPrompt() {
    wx.showToast({ title: '设备未连接', icon: 'none' });
  },

  audioErrorText(error) {
    const labels = {
      ESP_ERR_NOT_SUPPORTED: 'WAV 格式不支持，请转换为单声道 16-bit PCM、16/24 kHz',
      ESP_ERR_NOT_FOUND: '音频文件不存在',
      ESP_ERR_INVALID_STATE: 'SD 卡或音频硬件尚未就绪',
      ESP_ERR_INVALID_SIZE: 'WAV 文件数据不完整',
      ESP_ERR_INVALID_RESPONSE: '文件不是有效的 RIFF/WAVE'
    };
    return labels[error] || (error && error !== 'ESP_OK' ? error : '');
  },

  refreshStatus() {
    if (!this.data.configured || this.statusRefreshing) return Promise.resolve(false);
    this.statusRefreshing = true;
    return api.getAudioStatus().then((result) => {
      const becameOnline = !this.data.online;
      const audio = result.audio || result;
      const hardware = audio.hardware || {};
      const controlOnly = !!audio.control_only ||
        (audio.ready !== false && hardware.output_enabled === false);
      const state = String(audio.state || 'STOPPED').toUpperCase();
      const file = audio.file || {};
      const audioError = this.audioErrorText(audio.last_error);
      if (state === 'ERROR' && audioError && this.lastAudioError !== audio.last_error) {
        this.lastAudioError = audio.last_error;
        wx.showToast({ title: audioError, icon: 'none', duration: 3500 });
      } else if (state !== 'ERROR') {
        this.lastAudioError = '';
      }
      const nextData = {
        online: true,
        controlOnly,
        statusText: state === 'ERROR' && audioError
          ? `音频错误：${audioError}`
          : (controlOnly ? '设备在线 · 无声同步测试模式' : '设备在线 · SD 音乐可用'),
        volume: api.deviceVolumeToUi(hardware.volume_percent === undefined ? audio.volume : hardware.volume_percent),
        muted: hardware.muted === undefined ? !!audio.muted : !!hardware.muted,
        activeFile: file.name || '',
        filePaused: state === 'FILE_PAUSED',
        sdPresent: audio.sd_present === undefined ? this.data.sdPresent : !!audio.sd_present
      };
      if (this.volumeChanging) delete nextData.volume;
      this.setData(nextData);
      if (becameOnline) this.loadFiles(1);
      return true;
    }).catch((err) => {
      this.filesRequestId = (this.filesRequestId || 0) + 1;
      this.setData({
        online: false,
        controlOnly: false,
        statusText: api.errorMessage(err, '设备连接失败'),
        sdPresent: false,
        sdText: '暂无曲库信息',
        files: [],
        total: 0,
        totalPages: 0,
        filesLoading: false,
        activeFile: '',
        filePaused: false,
        emptyText: '没有找到可播放的 WAV 文件'
      });
      return false;
    }).finally(() => {
      this.statusRefreshing = false;
    });
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

  onSearchInput(event) {
    this.setData({ searchInput: String(event.detail.value || '') });
  },

  searchFiles() {
    const search = String(this.data.searchInput || '').trim();
    if (utf8.encode(search).length > 64) {
      wx.showToast({ title: '搜索内容不能超过 64 字节', icon: 'none' });
      return;
    }
    if (!this.data.online) {
      this.showConnectPrompt();
      return;
    }
    this.setData({ activeSearch: search });
    this.loadFiles(1);
  },

  clearSearch() {
    this.setData({ searchInput: '', activeSearch: '' });
    if (this.data.online) {
      this.loadFiles(1);
    } else {
      this.setData({ emptyText: '没有找到可播放的 WAV 文件' });
      this.showConnectPrompt();
    }
  },

  refreshFiles() {
    if (this.data.filesLoading) return;
    if (!this.data.online) {
      this.showConnectPrompt();
      return;
    }
    this.loadFiles(this.data.page);
  },

  loadFiles(page) {
    if (!this.data.online) return Promise.resolve(false);
    const requestedPage = Math.max(1, Number(page) || 1);
    const requestId = (this.filesRequestId || 0) + 1;
    this.filesRequestId = requestId;
    this.setData({ filesLoading: true });
    return api.getAudioFiles(requestedPage, this.data.activeSearch).then((result) => {
      if (requestId !== this.filesRequestId || !this.data.online) return;
      const files = Array.isArray(result.files)
        ? result.files.map((item) => typeof item === 'string' ? item : item.name).filter(Boolean)
        : [];
      const present = result.sd_present !== false;
      const total = Number(result.total || 0);
      const totalPages = Number(result.total_pages || (total ? Math.ceil(total / 10) : 0));
      const currentPage = Number(result.page || requestedPage);
      this.setData({
        files,
        page: currentPage,
        total,
        totalPages,
        sdPresent: present,
        sdText: present ? 'SD 卡已就绪' : '未检测到 SD 卡',
        emptyText: this.data.activeSearch ? '没有找到相关 WAV 文件' : '没有找到可播放的 WAV 文件'
      });
    }).catch((err) => {
      if (requestId !== this.filesRequestId) return;
      this.setData({ sdPresent: false, files: [], total: 0, totalPages: 0, sdText: api.errorMessage(err, 'SD 卡不可用') });
    }).finally(() => {
      if (requestId === this.filesRequestId) this.setData({ filesLoading: false });
    });
  },

  previousPage() {
    if (!this.data.online) {
      this.showConnectPrompt();
      return;
    }
    if (!this.data.filesLoading && this.data.page > 1) {
      this.loadFiles(this.data.page - 1);
    }
  },

  nextPage() {
    if (!this.data.online) {
      this.showConnectPrompt();
      return;
    }
    if (!this.data.filesLoading && this.data.page < this.data.totalPages) {
      this.loadFiles(this.data.page + 1);
    }
  },

  playFile(event) {
    const name = String(event.currentTarget.dataset.name || '');
    if (!name) return;
    this.runCommand('file', () => api.playAudioFile(name)).then((ok) => {
      if (ok) this.setData({ activeFile: name, filePaused: false });
    });
  },

  pauseFile() {
    if (!this.data.activeFile) return;
    this.runCommand('file', () => api.pauseAudioFile()).then((ok) => {
      if (ok) this.setData({ filePaused: !this.data.filePaused });
    });
  },

  stopFile() {
    if (!this.data.activeFile) return;
    this.runCommand('file', () => api.stopAudioFile()).then((ok) => {
      if (ok) this.setData({ activeFile: '', filePaused: false });
    });
  }
});
