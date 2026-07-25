Page({
  data: {
    records: [],
    selectedRecord: null
  },

  onShow() {
    this.loadRecords();
  },

  loadRecords() {
    const records = wx.getStorageSync('practiceRecords') || [];
    this.setData({
      records,
      selectedRecord: this.data.selectedRecord
        ? records.find((item) => item.id === this.data.selectedRecord.id) || null
        : null
    });
  },

  saveRecords(records) {
    wx.setStorageSync('practiceRecords', records);
    this.setData({ records });
  },

  viewDetail(event) {
    const id = event.currentTarget.dataset.id;
    const record = this.data.records.find((item) => item.id === id);
    if (!record) return;
    this.setData({ selectedRecord: record });
  },

  closeDetail() {
    this.setData({ selectedRecord: null });
  },

  noop() {},

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
