const api = require('../../utils/api');
const scoreLibrary = require('../../utils/score_library');
const practiceAdvice = require('../../utils/practice_advice');
const performanceChart = require('../../utils/performance_chart');
const practiceRecord = require('../../utils/practice_record');

function emptyResult() {
  return {
    totalScore: '-',
    pitchScore: '-',
    rhythmScore: '-',
    fluencyScore: '-',
    completeScore: '-',
    level: '',
    scoreNotice: '',
    confidenceText: '',
    targetCount: 0,
    startOffset: null,
    tempoScale: null,
    alignmentMethod: '',
    startAnchorTargetIndex: 0,
    startAnchorPlayedIndex: 0,
    leadingMissingCount: 0,
    leadingExtraCount: 0,
    alignmentOriginLocked: false,
    tempoSampleCount: 0,
    details: []
  };
}

const { formatPracticeAdvice } = practiceAdvice;

Page({
  data: {
    scoreTitle: '未选择乐谱',
    currentScore: null,
    status: '未开始',
    statusClass: '',
    isRecording: false,
    isStarting: false,
    isAdviceLoading: false,
    adviceState: 'none',
    adviceButtonText: '练习建议',
    adviceMessage: '结束练习后会自动生成建议',
    currentPracticeSessionId: '',
    currentAdviceText: '',
    preparationValid: false,
    preparationChanging: false,
    preparationRevision: 0,
    preparationPhase: 'idle',
    preparationMessage: '选择乐谱后可设置显示和跟谱方式',
    notationType: 'staff',
    practiceMode: 'follow',
    inputChanging: false,
    selectedInput: 'usb_midi',
    activeInput: 'usb_midi',
    inputLocked: false,
    usbMidiConnected: false,
    audioS3Connected: false,
    audioS3Implemented: false,
    creatorActive: false,
    result: emptyResult(),
    performanceTargetNotes: [],
    liveStatus: {
      online: false,
      state: '未连接',
      phase: '未开始',
      phaseClass: '',
      rms: '-',
      noise: '-',
      threshold: '-',
      note: '-',
      progress: '0 / 0',
      message: '请先在设备页完成连接'
    }
  },

  onShow() {
    this.pageVisible = true;
    const app = getApp();
    const current = scoreLibrary.getCurrentScore() || app.globalData.currentScore;
    const currentTargetNotes = performanceChart.normalizeTargetNotes(
      current && current.notes
    );
    const keepSessionTargets =
      (this.data.isRecording || this.data.status === '已完成') &&
      this.data.performanceTargetNotes.length > 0;
    this.setData({
      scoreTitle: current ? current.title : '未选择乐谱',
      currentScore: current || null,
      performanceTargetNotes: keepSessionTargets
        ? this.data.performanceTargetNotes
        : currentTargetNotes
    });
    this.startStatusPolling();
  },

  onHide() {
    this.pageVisible = false;
    this.stopStatusPolling();
  },

  onUnload() {
    this.pageVisible = false;
    this.stopStatusPolling();
  },

  startStatusPolling() {
    this.stopStatusPolling();
    if (!this.pageVisible) return;
    this.refreshLiveStatus();
    this.statusTimer = setInterval(() => this.refreshLiveStatus(), 1000);
  },

  stopStatusPolling() {
    if (this.statusTimer) {
      clearInterval(this.statusTimer);
      this.statusTimer = null;
    }
  },

  fixed(value, digits = 1) {
    const number = Number(value);
    return Number.isFinite(number) ? number.toFixed(digits) : '-';
  },

  phaseLabel(phase) {
    if (phase === 'CALIBRATING') return '环境音评估';
    if (phase === 'WAITING') return '等待演奏';
    if (phase === 'RECORDING') return '正在记录';
    return '未开始';
  },

  phaseClass(phase, state) {
    if (state === 'RECORDING' && phase === 'RECORDING') return 'recording';
    if (state === 'RECORDING') return 'warn';
    if (state === 'FINISHED') return 'finished';
    return '';
  },

  applyPreparationResult(result) {
    const preparation = result && (result.preparation || result);
    if (!preparation || !preparation.valid) return false;
    this.setData({
      preparationValid: true,
      preparationRevision: Number(preparation.revision || 0),
      preparationPhase: preparation.phase || 'prepared',
      preparationMessage: preparation.message || '乐谱准备就绪',
      notationType: preparation.notation_type || 'staff',
      practiceMode: preparation.mode || 'follow',
      selectedInput: preparation.input_source || this.data.selectedInput,
      scoreTitle: preparation.title || this.data.scoreTitle
    });
    return true;
  },

  refreshLiveStatus() {
    if (this.statusRefreshInFlight) {
      return Promise.resolve(false);
    }
    if (!api.getBaseUrl()) {
      this.setData({
        liveStatus: {
          online: false,
          state: '未连接',
          phase: '未开始',
          phaseClass: '',
          rms: '-',
          noise: '-',
          threshold: '-',
          note: '-',
          progress: '0 / 0',
          message: '请先在设备页完成蓝牙和 Wi-Fi 连接'
        }
      });
      return Promise.resolve(false);
    }

    this.statusRefreshInFlight = true;
    return Promise.all([
      api.getStatus(),
      api.getCreatorStatus().catch(() => null),
      api.getPracticePreparation().catch(() => null)
    ])
      .then(([status, creator, preparationResult]) => {
        const preparation = preparationResult &&
          (preparationResult.preparation || preparationResult);
        const preparationValid = !!(preparation && preparation.valid);
        const practiceState = String(status.practice_state || '').toUpperCase();
        const legacyState = String(status.state || '').toUpperCase();
        const knownPracticeStates = ['IDLE', 'RECORDING', 'SCORING', 'READY', 'ERROR', 'FINISHED'];
        const state = practiceState || (
          knownPracticeStates.indexOf(legacyState) >= 0 ? legacyState : 'IDLE'
        );
        const phase = status.record_phase || (state === 'RECORDING' ? 'RECORDING' : 'IDLE');
        const creatorActive = !!(creator && creator.active);
        const nextStatus = {
          online: true,
          state: creatorActive ? 'CREATOR' : state,
          phase: creatorActive ? '创作者模式运行中' : this.phaseLabel(phase),
          phaseClass: creatorActive ? 'warn' : this.phaseClass(phase, state),
          rms: this.fixed(status.rms, 1),
          noise: this.fixed(status.noise_floor, 1),
          threshold: this.fixed(status.dynamic_threshold, 1),
          note: status.note || status.last_note || '-',
          progress: `${Number(status.played_count || 0)} / ${Number(status.target_count || 0)}`,
          message: creatorActive
            ? '设备正在录制创作，结束或取消后才能开始评分练习'
            : (status.message || '设备状态正常')
        };
        const nextData = {
          liveStatus: nextStatus,
          preparationValid,
          selectedInput: preparationValid
            ? (preparation.input_source || 'usb_midi')
            : (status.selected_input || 'usb_midi'),
          activeInput: status.active_input || status.selected_input || 'usb_midi',
          inputLocked: !!status.input_locked,
          usbMidiConnected: !!status.usb_midi_connected,
          audioS3Connected: !!status.audio_s3_connected,
          audioS3Implemented: !!status.audio_s3_implemented,
          creatorActive
        };
        if (preparationValid) {
          nextData.preparationValid = true;
          nextData.preparationRevision =
            Number(preparation.revision || 0);
          nextData.preparationPhase = preparation.phase || 'prepared';
          nextData.preparationMessage =
            preparation.message || '乐谱准备就绪';
          nextData.notationType =
            preparation.notation_type || 'staff';
          nextData.practiceMode = preparation.mode || 'follow';
          nextData.scoreTitle =
            preparation.title || this.data.scoreTitle;
        }
        if (state === 'RECORDING') {
          if (!this.data.isRecording) {
            practiceRecord.rememberPendingContext({
              scoreTitle: nextData.scoreTitle || this.data.scoreTitle,
              targetNotes: this.data.performanceTargetNotes
            });
          }
          nextData.isRecording = true;
          nextData.status = nextStatus.phase;
          nextData.statusClass = nextStatus.phaseClass;
        } else if (state === 'SCORING') {
          nextData.isRecording = false;
          nextData.status = '正在评分';
          nextData.statusClass = 'warn';
        } else if (state === 'READY') {
          nextData.isRecording = false;
          nextData.status = '评分已完成，正在保存';
          nextData.statusClass = 'warn';
        } else if (this.data.isRecording && state !== 'RECORDING') {
          nextData.isRecording = false;
        }
        this.setData(nextData);
        const resultSync = state === 'READY'
          ? this.syncCompletedPracticeResult(status)
          : Promise.resolve(false);
        return resultSync.then(() => this.applyAdviceSummary(status));
      })
      .catch(() => {
        this.setData({
          liveStatus: {
            online: false,
            state: '离线',
            phase: '不可用',
            phaseClass: 'bad',
            rms: '-',
            noise: '-',
            threshold: '-',
            note: '-',
            progress: '0 / 0',
            message: '无法连接到设备，请检查设备地址和网络'
          }
        });
      })
      .finally(() => {
        this.statusRefreshInFlight = false;
      });
  },

  savePracticeRecords(records) {
    try {
      wx.setStorageSync('practiceRecords', records);
      return true;
    } catch (err) {
      if (!this.storageWarningVisible) {
        this.storageWarningVisible = true;
        wx.showModal({
          title: '记录保存失败',
          content: '小程序本地存储空间不足，已有练习记录不会被删除。',
          showCancel: false,
          complete: () => { this.storageWarningVisible = false; }
        });
      }
      return false;
    }
  },

  syncCompletedPracticeResult(status) {
    if (this.completionInFlight) return Promise.resolve(false);
    const sessionId = String(status && status.practice_session_id || '');
    const records = wx.getStorageSync('practiceRecords') || [];
    const existing = practiceRecord.findBySession(records, sessionId);
    if (existing) {
      if (this.data.status !== '已完成' ||
          this.data.currentPracticeSessionId !== sessionId) {
        this.finishPractice(existing, { automatic: true, silent: true });
      }
      practiceRecord.clearPendingContext();
      return Promise.resolve(false);
    }

    this.completionInFlight = true;
    return api.getResult()
      .then((apiResult) => {
        if (!apiResult || apiResult.ready === false) return false;
        const result = this.normalizeResult(apiResult);
        if (sessionId && result.practiceSessionId &&
            sessionId !== result.practiceSessionId) {
          throw new Error('设备返回了其他练习会话的评分');
        }
        this.finishPractice(result, { automatic: true });
        return true;
      })
      .catch(() => {
        this.setData({
          status: '评分已完成，记录同步重试中',
          statusClass: 'warn',
          isRecording: false
        });
        return false;
      })
      .finally(() => {
        this.completionInFlight = false;
      });
  },

  updateAdviceRecord(sessionId, patch) {
    const records = wx.getStorageSync('practiceRecords') || [];
    const result = practiceAdvice.updateRecordBySession(records, sessionId, patch);
    if (!result.updated) return false;
    return this.savePracticeRecords(result.records);
  },

  applyAdviceSummary(status) {
    const state = practiceAdvice.normalizeAdviceState(status && status.advice_state);
    const sessionId = String(status && status.practice_session_id || '');
    const message = String(status && (
      status.advice_message || status.practice_message || status.message
    ) || '');
    this.setData({
      adviceState: state,
      adviceButtonText: practiceAdvice.adviceButtonText(state),
      adviceMessage: message || this.data.adviceMessage,
      currentPracticeSessionId: sessionId || this.data.currentPracticeSessionId
    });

    if (sessionId) {
      const records = wx.getStorageSync('practiceRecords') || [];
      const mismatched = practiceAdvice.markMismatchedPending(records, sessionId);
      if (mismatched.updated) this.savePracticeRecords(mismatched.records);
    }

    if (state === 'ready') return this.syncReadyAdvice(sessionId, false);
    if (['skipped_offline', 'failed'].includes(state)) {
      this.updateAdviceRecord(sessionId, {
        adviceStatus: state,
        adviceMessage: message || (state === 'skipped_offline'
          ? '本次练习结束时设备未联网，未生成建议'
          : '练习建议生成失败')
      });
    }
    return Promise.resolve(false);
  },

  syncReadyAdvice(sessionId, showAfterSync) {
    const id = String(sessionId || '');
    if (!id) return Promise.resolve(false);
    const records = wx.getStorageSync('practiceRecords') || [];
    const existing = records.find((record) => record.practiceSessionId === id);
    if (existing && existing.adviceStatus === 'ready' && existing.adviceText) {
      this.setData({ currentAdviceText: existing.adviceText });
      if (showAfterSync) {
        wx.showModal({
          title: 'AI 练习建议',
          content: existing.adviceText,
          showCancel: false
        });
      }
      return Promise.resolve(true);
    }
    if (this.adviceFetchSession === id) return this.adviceFetchPromise || Promise.resolve(false);

    this.adviceFetchSession = id;
    this.adviceFetchPromise = api.getPracticeAdvice()
      .then((response) => {
        if (!response || response.state !== 'ready' ||
            response.session_id !== id || !response.advice) {
          throw new Error('设备返回的练习建议与当前记录不匹配');
        }
        const content = formatPracticeAdvice(response.advice);
        const saved = this.updateAdviceRecord(id, {
          adviceStatus: 'ready',
          advice: response.advice,
          adviceText: content,
          adviceMessage: '练习建议已生成'
        });
        this.setData({
          adviceState: 'ready',
          adviceButtonText: practiceAdvice.adviceButtonText('ready'),
          adviceMessage: '练习建议已生成',
          currentPracticeSessionId: id,
          currentAdviceText: content
        });
        if (showAfterSync) {
          wx.showModal({
            title: 'AI 练习建议',
            content,
            showCancel: false
          });
        } else if (saved && this.lastAdviceReadyToast !== id) {
          this.lastAdviceReadyToast = id;
          wx.showToast({ title: '练习建议已生成', icon: 'success' });
        }
        return true;
      })
      .catch((err) => {
        this.setData({
          adviceMessage: api.errorMessage(err, '暂时无法读取练习建议')
        });
        if (showAfterSync) {
          wx.showModal({
            title: '建议暂不可用',
            content: api.errorMessage(err, '暂时无法读取练习建议'),
            showCancel: false
          });
        }
        return false;
      })
      .finally(() => {
        if (this.adviceFetchSession === id) {
          this.adviceFetchSession = '';
          this.adviceFetchPromise = null;
        }
      });
    return this.adviceFetchPromise;
  },

  selectInputSource(event) {
    const source = event.currentTarget.dataset.source;
    if (!source || this.data.inputChanging || this.data.inputLocked) return;
    this.setData({ inputChanging: true });
    const action = this.data.preparationValid
      ? api.updatePracticePreparation({ input_source: source })
      : api.selectInputSource(source);
    action
      .then((status) => {
        if (this.data.preparationValid) {
          this.applyPreparationResult(status);
          wx.showToast({ title: '准备选项已同步', icon: 'success' });
          return;
        }
        this.setData({
          selectedInput: status.selected_input || source,
          activeInput: status.active_input || source,
          inputLocked: !!status.input_locked,
          usbMidiConnected: !!status.usb_midi_connected,
          audioS3Connected: !!status.audio_s3_connected,
          audioS3Implemented: !!status.audio_s3_implemented
        });
        wx.showToast({ title: '输入源已保存', icon: 'success' });
      })
      .catch((err) => {
        wx.showModal({
          title: '无法切换输入源',
          content: api.errorMessage(err, '输入源切换失败'),
          showCancel: false
        });
      })
      .finally(() => this.setData({ inputChanging: false }));
  },

  updatePreparationOption(patch) {
    if (!this.data.preparationValid || this.data.preparationChanging) {
      return Promise.resolve(false);
    }
    this.setData({ preparationChanging: true });
    return api.updatePracticePreparation(patch)
      .then((result) => {
        this.applyPreparationResult(result);
        return true;
      })
      .catch((err) => {
        wx.showModal({
          title: '准备选项同步失败',
          content: api.errorMessage(err, '无法更新设备屏幕'),
          showCancel: false
        });
        return false;
      })
      .finally(() => this.setData({ preparationChanging: false }));
  },

  selectNotationType(event) {
    const notationType = event.currentTarget.dataset.notation;
    if (notationType !== 'numbered' && notationType !== 'staff') return;
    this.updatePreparationOption({ notation_type: notationType });
  },

  buildEmptyResult() {
    return emptyResult();
  },

  hasValidCurrentScore() {
    const score = this.data.currentScore;
    return !!score && Array.isArray(score.notes) && score.notes.length > 0;
  },

  startPractice() {
    this.launchPracticeMode('follow');
  },

  previewScore() {
    if (!this.data.preparationValid) {
      wx.showModal({
        title: '请先选择设备乐谱',
        content: '预览会调用设备屏幕的只读界面，请先从设备 SD 卡选择一份乐谱。',
        showCancel: false
      });
      return;
    }
    this.launchPracticeMode('read_only');
  },

  launchPracticeMode(mode) {
    if (this.data.isRecording || this.data.isStarting) return;
    if (this.data.creatorActive) {
      wx.showModal({
        title: '创作者模式正在运行',
        content: '请先在创作者模式页面结束并保存，或取消当前创作。',
        showCancel: false
      });
      return;
    }
    if (!this.data.preparationValid && !this.hasValidCurrentScore()) {
      wx.showModal({
        title: '请先选择有效乐谱',
        content: '当前乐谱没有 notes，不能评分。请先在乐谱页同步一份 JSON/MIDI 或 AI 识谱成功的乐谱。',
        showCancel: false
      });
      return;
    }

    const restartingCompletedPractice =
      this.data.preparationValid && mode === 'follow' &&
      this.data.status === '已完成';
    const currentTargetNotes = performanceChart.normalizeTargetNotes(
      this.data.currentScore && this.data.currentScore.notes
    );
    const sessionTargetNotes = restartingCompletedPractice &&
      this.data.performanceTargetNotes.length
      ? performanceChart.compactTargetNotes(this.data.performanceTargetNotes)
      : currentTargetNotes;
    wx.showLoading({ title: '启动练习中' });
    this.setData({
      isStarting: true,
      performanceTargetNotes: sessionTargetNotes
    });
    const startAction = restartingCompletedPractice
      ? api.restartPreparedPractice()
      : this.data.preparationValid
      ? api.updatePracticePreparation({ mode })
          .then((preparation) => {
            this.applyPreparationResult(preparation);
            return api.startPreparedPractice();
          })
      : api.startPractice();
    startAction
      .then((result) => {
        if (!result || result.ok === false) {
          throw new Error(api.messageText(result && result.message, '开始练习失败'));
        }
        if (this.data.preparationValid) {
          this.applyPreparationResult(result);
        }
        const readOnly = this.data.preparationValid && mode === 'read_only';
        if (!readOnly) {
          const preparation = result && (result.preparation || result);
          practiceRecord.rememberPendingContext({
            scoreTitle: preparation && preparation.title
              ? preparation.title
              : this.data.scoreTitle,
            targetNotes: sessionTargetNotes
          });
        }
        this.setData({
          status: readOnly ? '只读看谱' : '正在记录',
          statusClass: readOnly ? 'finished' : 'recording',
          isRecording: !readOnly,
          result: this.buildEmptyResult(),
          adviceState: readOnly ? 'none' : 'waiting_score',
          adviceButtonText: '练习建议',
          adviceMessage: readOnly
            ? '只读看谱不会生成练习建议'
            : '结束练习后会自动生成建议',
          currentPracticeSessionId: '',
          currentAdviceText: ''
        });
        this.refreshLiveStatus();
        wx.showToast({
          title: readOnly ? '屏幕已显示乐谱' : '已开始跟谱',
          icon: 'success'
        });
      })
      .catch((err) => {
        wx.showModal({
          title: '无法开始练习',
          content: api.errorMessage(err, '无法开始练习'),
          showCancel: false
        });
      })
      .finally(() => {
        this.setData({ isStarting: false });
        wx.hideLoading();
      });
  },

  stopPractice() {
    if (this.completionInFlight) return;
    this.completionInFlight = true;
    wx.showLoading({ title: '生成评分中' });
    api.stopPractice()
      .then((apiResult) => this.finishPractice(this.normalizeResult(apiResult)))
      .catch((err) => {
        this.setData({
          status: '未开始',
          statusClass: '',
          isRecording: false,
          result: this.buildEmptyResult()
        });
        wx.showModal({
          title: '评分失败',
          content: `${api.errorMessage(err, '评分失败')}\n如果串口显示 target=0 或 frames=0，请先同步有效乐谱，并确认设备真的开始录音。`,
          showCancel: false
        });
      })
      .finally(() => {
        this.completionInFlight = false;
        wx.hideLoading();
      });
  },

  normalizeResult(result) {
    try {
      return practiceRecord.normalizeResult(result);
    } catch (error) {
      throw new Error(api.messageText(result && result.message, error.message));
    }
  },

  finishPractice(result, options) {
    const settings = options || {};
    const records = wx.getStorageSync('practiceRecords') || [];
    let record = practiceRecord.findBySession(
      records, result.practiceSessionId
    );
    let stored = !!record;
    if (!record) {
      record = practiceRecord.buildRecord(result, {
        scoreTitle: this.data.scoreTitle,
        targetNotes: this.data.performanceTargetNotes
      });
      records.unshift(record);
      stored = this.savePracticeRecords(records);
    }
    if (stored) practiceRecord.clearPendingContext();
    const storedAdviceState = practiceAdvice.normalizeAdviceState(
      record.adviceStatus
    );
    const adviceState = storedAdviceState === 'pending'
      ? 'running'
      : storedAdviceState;
    this.setData({
      status: '已完成',
      statusClass: 'finished',
      isRecording: false,
      result,
      adviceState,
      adviceButtonText: practiceAdvice.adviceButtonText(adviceState),
      adviceMessage: record.adviceMessage,
      currentPracticeSessionId: result.practiceSessionId,
      currentAdviceText: record.adviceText || '',
      performanceTargetNotes: performanceChart.compactTargetNotes(record.targetNotes)
    });
    this.refreshLiveStatus();
    if (!settings.silent) {
      wx.showToast({
        title: settings.automatic ? '已自动保存记录' : '评分完成',
        icon: 'success'
      });
    }
  },

  resetPractice() {
    if (this.data.isRecording || this.data.isStarting) return;
    if (!this.data.preparationValid) {
      wx.showModal({
        title: '无法重新练习',
        content: '设备没有保留当前乐谱，请重新同步一份有效乐谱。',
        showCancel: false
      });
      return;
    }

    wx.showLoading({ title: '重新开始中' });
    const restartTargetNotes = this.data.performanceTargetNotes.length
      ? performanceChart.compactTargetNotes(this.data.performanceTargetNotes)
      : performanceChart.normalizeTargetNotes(
        this.data.currentScore && this.data.currentScore.notes
      );
    this.setData({
      isStarting: true,
      performanceTargetNotes: restartTargetNotes
    });
    api.restartPreparedPractice()
      .then((result) => {
        if (!result || result.ok === false) {
          throw new Error(api.messageText(result && result.message, '重新练习失败'));
        }
        this.applyPreparationResult(result);
        practiceRecord.rememberPendingContext({
          scoreTitle: this.data.scoreTitle,
          targetNotes: restartTargetNotes
        });
        this.setData({
          status: '正在记录',
          statusClass: 'recording',
          isRecording: true,
          result: this.buildEmptyResult(),
          adviceState: 'waiting_score',
          adviceButtonText: '练习建议',
          adviceMessage: '结束练习后会自动生成建议',
          currentPracticeSessionId: '',
          currentAdviceText: ''
        });
        this.refreshLiveStatus();
        wx.showToast({ title: '已重新开始', icon: 'success' });
      })
      .catch((err) => {
        wx.showModal({
          title: '无法重新练习',
          content: api.errorMessage(err, '设备未能重新开始当前乐谱'),
          showCancel: false
        });
      })
      .finally(() => {
        this.setData({ isStarting: false });
        wx.hideLoading();
      });
  },

  viewPracticeAdvice() {
    const state = practiceAdvice.normalizeAdviceState(this.data.adviceState);
    if (state === 'ready') {
      if (this.data.currentAdviceText) {
        wx.showModal({
          title: 'AI 练习建议',
          content: this.data.currentAdviceText,
          showCancel: false
        });
        return Promise.resolve(true);
      }
      this.setData({ isAdviceLoading: true });
      wx.showLoading({ title: '读取建议中' });
      return this.syncReadyAdvice(this.data.currentPracticeSessionId, true)
        .finally(() => {
          wx.hideLoading();
          this.setData({ isAdviceLoading: false });
        });
    }
    if (state === 'running') {
      wx.showModal({
        title: '建议生成中',
        content: '建议正在后台生成，请稍等。你可以先查看本次评分。',
        showCancel: false
      });
      return false;
    }
    if (state === 'waiting_score') {
      wx.showModal({
        title: '练习建议',
        content: this.data.isRecording
          ? '请先结束练习，评分完成后设备会自动生成建议。'
          : '正在等待本地评分完成，请稍等。',
        showCancel: false
      });
      return false;
    }
    if (['skipped_offline', 'failed', 'sync_missed'].includes(state)) {
      wx.showModal({
        title: '建议未生成',
        content: this.data.adviceMessage || '本次没有可查看的练习建议。',
        showCancel: false
      });
      return false;
    }
    wx.showModal({
      title: '练习建议',
      content: '请先完成一次练习。',
      showCancel: false
    });
    return false;
  },

  requestAiScore() {
    return this.viewPracticeAdvice();
  },

  formatPracticeAdvice
});
