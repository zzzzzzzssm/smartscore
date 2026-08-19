const practiceAdvice = require('../../utils/practice_advice');
const api = require('../../utils/api');

function downloadPhotoBatch(items, sessionId) {
  const photos = (items || []).map((item) => Object.assign({}, item, { path: '', error: '' }));
  let cursor = 0;
  const worker = () => {
    const position = cursor;
    cursor += 1;
    if (position >= photos.length) return Promise.resolve();
    const photo = photos[position];
    return api.downloadPracticePhoto(sessionId, photo.index)
      .then((path) => { photo.path = path; })
      .catch((err) => { photo.error = api.errorMessage(err, '读取失败'); })
      .then(worker);
  };
  return Promise.all([worker(), worker()]).then(() => photos);
}

function decorateRecord(record) {
  const adviceStatus = practiceAdvice.normalizeAdviceState(record.adviceStatus);
  return Object.assign({}, record, {
    adviceStatus,
    adviceStatusLabel: practiceAdvice.adviceStatusLabel(adviceStatus),
    adviceStatusClass: adviceStatus === 'ready'
      ? 'ready'
      : (['waiting_score', 'running'].includes(adviceStatus) ? 'pending' : 'missing')
  });
}

Page({
  data: {
    records: [],
    selectedRecord: null,
    photos: [],
    photosLoading: false,
    photoMessage: '',
    photoPage: 1,
    photoTotal: 0,
    photoTotalPages: 0
  },

  onShow() {
    this.loadRecords();
  },

  loadRecords() {
    const records = (wx.getStorageSync('practiceRecords') || []).map(decorateRecord);
    this.setData({
      records,
      selectedRecord: this.data.selectedRecord
        ? records.find((item) => item.id === this.data.selectedRecord.id) || null
        : null
    });
  },

  saveRecords(records) {
    wx.setStorageSync('practiceRecords', records);
    this.setData({ records: records.map(decorateRecord) });
  },

  viewDetail(event) {
    const id = event.currentTarget.dataset.id;
    const record = this.data.records.find((item) => item.id === id);
    if (!record) return;
    this.setData({
      selectedRecord: record,
      photos: [],
      photosLoading: false,
      photoMessage: record.practiceSessionId ? '正在连接摄像头板…' : '这条记录没有照片会话编号。',
      photoPage: 1,
      photoTotal: 0,
      photoTotalPages: 0
    });
    if (record.practiceSessionId) this.loadPhotos(1);
  },

  closeDetail() {
    this.photoRequestId = (this.photoRequestId || 0) + 1;
    this.setData({ selectedRecord: null, photos: [], photosLoading: false });
  },

  noop() {},

  viewAdvice(event) {
    const id = event.currentTarget.dataset.id;
    const record = this.data.records.find((item) => item.id === id);
    if (!record) return;
    wx.showModal({
      title: record.adviceStatus === 'ready' ? 'AI 练习建议' : '建议未生成',
      content: record.adviceStatus === 'ready'
        ? (record.adviceText || '这条建议内容暂不可用。')
        : (record.adviceMessage || '这次练习没有保存练习建议。'),
      showCancel: false
    });
  },

  loadPhotos(page) {
    const record = this.data.selectedRecord;
    if (!record || !record.practiceSessionId || this.data.photosLoading) return;
    const requestedPage = Math.max(1, Number(page) || 1);
    const requestId = (this.photoRequestId || 0) + 1;
    this.photoRequestId = requestId;
    this.setData({ photosLoading: true, photos: [], photoMessage: '正在读取摄像头照片…' });
    api.getPracticePhotos(record.practiceSessionId, requestedPage)
      .then((result) => {
        if (requestId !== this.photoRequestId) return null;
        const items = Array.isArray(result.photos) ? result.photos : [];
        this.setData({
          photoPage: Number(result.page || requestedPage),
          photoTotal: Number(result.total || 0),
          photoTotalPages: Number(result.total_pages || 0),
          photoMessage: items.length ? '正在加载照片…' : '这次练习没有保存照片。'
        });
        return downloadPhotoBatch(items, record.practiceSessionId);
      })
      .then((photos) => {
        if (!photos || requestId !== this.photoRequestId) return;
        const loaded = photos.filter((item) => item.path).length;
        this.setData({
          photos,
          photoMessage: loaded
            ? `已在线读取 ${loaded} 张照片`
            : (photos.length ? '照片读取失败，请检查摄像头板和 SD 卡。' : this.data.photoMessage)
        });
      })
      .catch((err) => {
        if (requestId !== this.photoRequestId) return;
        this.setData({
          photos: [],
          photoMessage: api.errorMessage(err, '摄像头板离线或照片不可用')
        });
      })
      .finally(() => {
        if (requestId === this.photoRequestId) this.setData({ photosLoading: false });
      });
  },

  previousPhotoPage() {
    if (!this.data.photosLoading && this.data.photoPage > 1) {
      this.loadPhotos(this.data.photoPage - 1);
    }
  },

  nextPhotoPage() {
    if (!this.data.photosLoading && this.data.photoPage < this.data.photoTotalPages) {
      this.loadPhotos(this.data.photoPage + 1);
    }
  },

  previewPhoto(event) {
    const path = String(event.currentTarget.dataset.path || '');
    if (!path) return;
    const urls = this.data.photos.map((item) => item.path).filter(Boolean);
    wx.previewImage({ current: path, urls });
  },

  renameRecord(event) {
    const id = event.currentTarget.dataset.id;
    const record = this.data.records.find((item) => item.id === id);
    if (!record) return;

    wx.showModal({
      title: '重命名记录',
      editable: true,
      placeholderText: '输入记录名称',
      content: record.scoreTitle,
      confirmText: '保存',
      success: (res) => {
        if (!res.confirm) return;
        const title = String(res.content || '').trim();
        if (!title) {
          wx.showToast({ title: '名称不能为空', icon: 'none' });
          return;
        }
        const records = this.data.records.map((item) => (
          item.id === id ? Object.assign({}, item, { scoreTitle: title }) : item
        ));
        this.saveRecords(records);
        this.setData({ selectedRecord: records.find((item) => item.id === id) || null });
        wx.showToast({ title: '已重命名', icon: 'success' });
      }
    });
  },

  deleteRecord(event) {
    const id = event.currentTarget.dataset.id;
    wx.showModal({
      title: '删除记录',
      content: '确认删除这次练习记录吗？',
      confirmText: '删除',
      confirmColor: '#dc2626',
      success: (res) => {
        if (!res.confirm) return;
        const records = this.data.records.filter((item) => item.id !== id);
        this.saveRecords(records);
        if (this.data.selectedRecord && this.data.selectedRecord.id === id) {
          this.closeDetail();
        }
        wx.showToast({ title: '已删除', icon: 'success' });
      }
    });
  }
});
