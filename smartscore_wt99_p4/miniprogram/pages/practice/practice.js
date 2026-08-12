const api = require('../../utils/api');
const scoreLibrary = require('../../utils/score_library');
const { formatTime } = require('../../utils/util');

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

function adviceText(value) {
  return typeof value === 'string' ? value.trim() : '';
}

function formatPracticeAdvice(result) {
  const summary = adviceText(result && result.summary);
  const focus = Array.isArray(result && result.focus) ? result.focus : [];
  const session = result && result.next_session;
  const steps = session && Array.isArray(session.steps) ? session.steps : [];
  if (!summary || focus.length === 0 || !session || steps.length === 0) {
    throw new Error('DeepSeek 返回的练习建议不完整');
  }

  const lines = [summary, '', '优先练习：'];
  focus.slice(0, 3).forEach((item, index) => {
    const practice = item && item.practice ? item.practice : {};
    const problem = adviceText(item && item.problem);
    const action = adviceText(practice.action);
    const target = adviceText(practice.target);
    if (!problem || !action || !target) {
      throw new Error('DeepSeek 返回的重点建议不完整');
    }
    lines.push(`${index + 1}. ${problem}`);
    const evidence = (Array.isArray(item.evidence) ? item.evidence : [])
      .map(adviceText)
      .filter(Boolean);
    if (evidence.length) lines.push(`依据：${evidence.join('；')}`);
    lines.push(`练习：${action}`);
    lines.push(`参数：${practice.bpm} BPM，${practice.minutes}分钟，重复${practice.repetitions}次`);
    lines.push(`达标：${target}`);
  });

  lines.push('', `下次练习（${session.total_minutes}分钟）：`);
  steps.forEach((step, index) => {
    const action = adviceText(step && step.action);
    if (!action) throw new Error('DeepSeek 返回的练习步骤不完整');
    lines.push(`${index + 1}. ${action}（${step.minutes}分钟）`);
  });

  const encouragement = adviceText(result.encouragement);
  if (encouragement) lines.push('', encouragement);
  const insufficient = (Array.isArray(result.insufficient_data)
    ? result.insufficient_data
    : [])
    .map(adviceText)
    .filter(Boolean);
  if (insufficient.length) {
    lines.push('', `数据不足：${insufficient.join('；')}`);
  }
  return lines.join('\n');
}

Page({
  data: {
    scoreTitle: '未选择乐谱',
    currentScore: null,
    status: '未开始',
    statusClass: '',
    isRecording: false,
    isStarting: false,
    isAdviceLoading: false,
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
    chartEmpty: true,
    chartHint: '结束练习后会显示标准谱、采集演奏和音高误差。',
    chartStats: {
      avgPitch: '-',
      maxPitch: '-',
      avgRhythm: '-',
      matched: '-'
    },
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

  onReady() {
    this.drawPracticeChart();
  },

  onShow() {
    this.pageVisible = true;
    const app = getApp();
    const current = scoreLibrary.getCurrentScore() || app.globalData.currentScore;
    this.setData({
      scoreTitle: current ? current.title : '未选择乐谱',
      currentScore: current || null
    }, () => this.drawPracticeChart());
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
    if (!this.pageVisible || this.data.isAdviceLoading) return;
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
    if (this.statusRefreshInFlight || this.data.isAdviceLoading) {
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
          nextData.isRecording = true;
          nextData.status = nextStatus.phase;
          nextData.statusClass = nextStatus.phaseClass;
        } else if (this.data.isRecording && state !== 'RECORDING') {
          nextData.isRecording = false;
        }
        this.setData(nextData);
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

  getMatchedDetails(result) {
    const details = result && Array.isArray(result.details) ? result.details : [];
    return details
      .filter((item) => (
        item.result !== 'uncertain' &&
        item.result !== 'extra' &&
        item.result !== 'retry' &&
        Number(item.ref_index) > 0 &&
        Number(item.target_midi) >= 0 &&
        Number(item.played_midi) >= 0 &&
        Number(item.target_start) >= 0 &&
        Number(item.played_start) >= 0
      ))
      .sort((left, right) => Number(left.ref_index) - Number(right.ref_index));
  },

  buildTargetNotes(result) {
    const details = result && Array.isArray(result.details) ? result.details : [];
    const resultTargets = new Map();
    details.forEach((item) => {
      const refIndex = Number(item.ref_index);
      const midi = Number(item.target_midi);
      const start = Number(item.target_start);
      if (refIndex <= 0 || !Number.isFinite(midi) || midi < 0 ||
          !Number.isFinite(start) || start < 0 || resultTargets.has(refIndex)) return;
      resultTargets.set(refIndex, {
        refIndex,
        midi,
        start,
        duration: Math.max(0.08, Number(item.target_duration || 0.25))
      });
    });
    const sortedResultTargets = Array.from(resultTargets.values())
      .sort((left, right) => left.refIndex - right.refIndex);
    const expectedTargetCount = Number(
      result && (result.targetCount || result.target_count)
    );
    if (sortedResultTargets.length &&
        (!Number.isFinite(expectedTargetCount) || expectedTargetCount <= 0 ||
         sortedResultTargets.length >= expectedTargetCount)) {
      return sortedResultTargets;
    }

    const current = this.data.currentScore;
    if (current && Array.isArray(current.notes) && current.notes.length) {
      return current.notes
        .map((note) => ({
          midi: Number(note.midi),
          start: Number(note.start),
          duration: Math.max(0.08, Number(note.duration || 0.25))
        }))
        .filter((note) => Number.isFinite(note.midi) && Number.isFinite(note.start));
    }
    return sortedResultTargets;
  },

  estimateVisualOffset(matched) {
    if (matched.some((item) => Number.isFinite(Number(item.aligned_played_start)))) {
      return 0;
    }
    const offsets = matched
      .map((item) => Number(item.played_start) - Number(item.target_start))
      .filter((value) => Number.isFinite(value))
      .sort((a, b) => a - b);
    if (!offsets.length) return 0;
    return offsets[Math.floor(offsets.length / 2)];
  },

  alignedPlayedStart(item, result, legacyOffset) {
    const aligned = Number(item.aligned_played_start);
    if (Number.isFinite(aligned)) return aligned;
    const raw = Number(item.played_start);
    const offset = Number(result && result.startOffset);
    const tempo = Number(result && result.tempoScale);
    if (Number.isFinite(raw) && Number.isFinite(offset) &&
        Number.isFinite(tempo) && tempo > 0) {
      return (raw - offset) / tempo;
    }
    return raw - Number(legacyOffset || 0);
  },

  updateChartStats(result, matched, targetNotes, visualOffset) {
    if (!matched.length) {
      this.setData({
        chartStats: {
          avgPitch: '-',
          maxPitch: '-',
          avgRhythm: '-',
          matched: `0 / ${targetNotes.length || '-'}`
        },
        chartHint: targetNotes.length
          ? '已载入标准谱；结束评分后会叠加采集演奏和音高误差。'
          : '结束练习后会显示标准谱、采集演奏和音高误差。',
        chartEmpty: !targetNotes.length
      });
      return;
    }

    const pitchErrors = matched.map((item) => Math.abs(Number(item.played_midi) - Number(item.target_midi)));
    const rhythmErrors = matched.map((item) => Math.abs(Number(item.time_error || 0)));
    const avgPitch = pitchErrors.reduce((sum, value) => sum + value, 0) / pitchErrors.length;
    const maxPitch = Math.max.apply(null, pitchErrors);
    const avgRhythm = rhythmErrors.reduce((sum, value) => sum + value, 0) / rhythmErrors.length;
    this.setData({
      chartStats: {
        avgPitch: `${avgPitch.toFixed(2)} 半音`,
        maxPitch: `${maxPitch.toFixed(2)} 半音`,
        avgRhythm: `${avgRhythm.toFixed(2)}s`,
        matched: `${matched.length} / ${result.targetCount || result.target_count || targetNotes.length || matched.length}`
      },
      chartHint: result.alignmentOriginLocked
        ? '已锁定乐谱开头和起奏时间原点；等待起奏不会把演奏吸附到后面的重复乐句。'
        : 'MIDI 已按音符顺序和演奏速度完成时间归一化，起奏等待不计分。',
      chartEmpty: false
    });
  },

  midiName(midi) {
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const value = Math.round(Number(midi));
    return `${names[((value % 12) + 12) % 12]}${Math.floor(value / 12) - 1}`;
  },

  drawPracticeChart() {
    const result = this.data.result || {};
    const matched = this.getMatchedDetails(result);
    const targetNotes = this.buildTargetNotes(result);
    const visualOffset = this.estimateVisualOffset(matched);
    this.updateChartStats(result, matched, targetNotes, visualOffset);

    const query = wx.createSelectorQuery().in(this);
    query.select('.performance-chart').boundingClientRect((rect) => {
      if (!rect || !rect.width || !rect.height) return;
      const ctx = wx.createCanvasContext('performanceChart', this);
      const width = rect.width;
      const height = rect.height;
      const pad = { left: 36, right: 16, top: 24, bottom: 34 };
      const errorBand = 58;
      const pitchTop = pad.top;
      const pitchBottom = height - pad.bottom - errorBand;
      const chartLeft = pad.left;
      const chartRight = width - pad.right;
      const chartWidth = Math.max(1, chartRight - chartLeft);
      const pitchHeight = Math.max(1, pitchBottom - pitchTop);
      const errorTop = pitchBottom + 22;
      const errorBottom = height - pad.bottom;

      const midiValues = [];
      targetNotes.forEach((note) => midiValues.push(Number(note.midi)));
      matched.forEach((item) => {
        midiValues.push(Number(item.target_midi));
        midiValues.push(Number(item.played_midi));
      });
      if (!midiValues.length) midiValues.push(60, 72);
      const midiMinRaw = Math.min.apply(null, midiValues);
      const midiMaxRaw = Math.max.apply(null, midiValues);
      const midiMin = Math.floor(midiMinRaw - 2);
      const midiMax = Math.ceil(midiMaxRaw + 2);
      const midiRange = Math.max(1, midiMax - midiMin);

      const targetEnd = targetNotes.reduce((max, note) => Math.max(max, Number(note.start) + Number(note.duration || 0.25)), 0);
      const playedEnd = matched.reduce((max, item) => {
        const start = this.alignedPlayedStart(item, result, visualOffset);
        return Math.max(max, start + Math.max(0.08, Number(item.played_duration || 0.25)));
      }, 0);
      const timeMax = Math.max(2, targetEnd, playedEnd);
      const xOf = (time) => chartLeft + (Math.max(0, Number(time || 0)) / timeMax) * chartWidth;
      const yOf = (midi) => pitchBottom - ((Number(midi) - midiMin) / midiRange) * pitchHeight;
      const yErr = (err) => {
        const clamped = Math.max(-3, Math.min(3, Number(err || 0)));
        return errorTop + ((3 - clamped) / 6) * (errorBottom - errorTop);
      };

      ctx.setFillStyle('#f8fbfd');
      ctx.fillRect(0, 0, width, height);
      ctx.setFontSize(10);
      ctx.setFillStyle('#667085');
      ctx.setStrokeStyle('#dfe8ef');
      ctx.setLineWidth(1);
      for (let i = 0; i <= 4; i += 1) {
        const y = pitchTop + (pitchHeight / 4) * i;
        ctx.beginPath();
        ctx.moveTo(chartLeft, y);
        ctx.lineTo(chartRight, y);
        ctx.stroke();
        const midi = midiMax - (midiRange / 4) * i;
        ctx.fillText(this.midiName(midi), 4, y + 3);
      }
      for (let i = 0; i <= 4; i += 1) {
        const x = chartLeft + (chartWidth / 4) * i;
        ctx.beginPath();
        ctx.moveTo(x, pitchTop);
        ctx.lineTo(x, errorBottom);
        ctx.stroke();
        ctx.fillText(`${Math.round((timeMax / 4) * i)}s`, x - 8, height - 10);
      }

      const drawTarget = () => {
        if (!targetNotes.length) return;
        ctx.setStrokeStyle('#2563eb');
        ctx.setLineWidth(3);
        ctx.setLineCap('round');
        targetNotes.forEach((note) => {
          const y = yOf(note.midi);
          ctx.beginPath();
          ctx.moveTo(xOf(note.start), y);
          ctx.lineTo(xOf(Number(note.start) + Number(note.duration || 0.25)), y);
          ctx.stroke();
        });
      };

      const drawPlayed = () => {
        if (!matched.length) return;
        ctx.setStrokeStyle('#0f766e');
        ctx.setFillStyle('#0f766e');
        ctx.setLineWidth(2);
        ctx.beginPath();
        matched.forEach((item, index) => {
          const x = xOf(this.alignedPlayedStart(item, result, visualOffset));
          const y = yOf(item.played_midi);
          if (index === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
        matched.forEach((item) => {
          const x = xOf(this.alignedPlayedStart(item, result, visualOffset));
          const y = yOf(item.played_midi);
          ctx.beginPath();
          ctx.arc(x, y, 3, 0, Math.PI * 2);
          ctx.fill();
        });
      };

      drawTarget();
      drawPlayed();

      ctx.setStrokeStyle('#94a3b8');
      ctx.setLineWidth(1);
      ctx.beginPath();
      ctx.moveTo(chartLeft, yErr(0));
      ctx.lineTo(chartRight, yErr(0));
      ctx.stroke();
      ctx.setFillStyle('#667085');
      ctx.fillText('+3', 8, errorTop + 6);
      ctx.fillText('0', 18, yErr(0) + 3);
      ctx.fillText('-3', 8, errorBottom + 3);

      if (matched.length) {
        ctx.setStrokeStyle('#dc2626');
        ctx.setLineWidth(2);
        ctx.beginPath();
        matched.forEach((item, index) => {
          const err = Number(item.played_midi) - Number(item.target_midi);
          const x = xOf(Number(item.target_start));
          const y = yErr(err);
          if (index === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
      }
      ctx.draw();
    }).exec();
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

    wx.showLoading({ title: '启动练习中' });
    this.setData({ isStarting: true });
    const restartingCompletedPractice =
      this.data.preparationValid && mode === 'follow' &&
      this.data.status === '已完成';
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
        this.setData({
          status: readOnly ? '只读看谱' : '正在记录',
          statusClass: readOnly ? 'finished' : 'recording',
          isRecording: !readOnly,
          result: this.buildEmptyResult()
        }, () => this.drawPracticeChart());
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
    wx.showLoading({ title: '生成评分中' });
    api.stopPractice()
      .then((apiResult) => this.finishPractice(this.normalizeResult(apiResult)))
      .catch((err) => {
        this.setData({
          status: '未开始',
          statusClass: '',
          isRecording: false,
          result: this.buildEmptyResult()
        }, () => this.drawPracticeChart());
        wx.showModal({
          title: '评分失败',
          content: `${api.errorMessage(err, '评分失败')}\n如果串口显示 target=0 或 frames=0，请先同步有效乐谱，并确认设备真的开始录音。`,
          showCancel: false
        });
      })
      .finally(() => wx.hideLoading());
  },

  normalizeResult(result) {
    if (!result || result.ok === false) {
      throw new Error(api.messageText(result && result.message, '评分失败'));
    }

    const scorable = result.scorable !== false;
    const referenceOnly = result.score_status === 'reference';
    const inputConfidence = Number(result.input_confidence);
    return {
      totalScore: scorable ? result.total_score : '-',
      pitchScore: result.pitch_score,
      rhythmScore: result.rhythm_evaluable === false ? '-' : result.rhythm_score,
      fluencyScore: result.fluency_score === undefined ? '-' : result.fluency_score,
      completeScore: result.complete_score,
      level: result.level || '',
      scoreNotice: !scorable
        ? '本次麦克风识别证据不足，未生成正式分数'
        : (referenceOnly ? '麦克风识别可信度一般，本次分数仅供参考' : ''),
      confidenceText: Number.isFinite(inputConfidence) && result.input_source === 'audio_s3'
        ? `输入可信度 ${Math.round(inputConfidence * 100)}%`
        : '',
      targetCount: Number(result.target_count || 0),
      startOffset: Number(result.start_offset || 0),
      tempoScale: Number(result.tempo_scale || 1),
      alignmentMethod: result.alignment_method || '',
      startAnchorTargetIndex: Number(result.start_anchor_target_index || 0),
      startAnchorPlayedIndex: Number(result.start_anchor_played_index || 0),
      leadingMissingCount: Number(result.leading_missing_count || 0),
      leadingExtraCount: Number(result.leading_extra_count || 0),
      alignmentOriginLocked: result.alignment_origin_locked === true,
      tempoSampleCount: Number(result.tempo_sample_count || 0),
      details: Array.isArray(result.details) ? result.details : []
    };
  },

  finishPractice(result) {
    const record = {
      id: `record_${Date.now()}`,
      scoreTitle: this.data.scoreTitle,
      practicedAt: formatTime(new Date()),
      totalScore: result.totalScore,
      pitchScore: result.pitchScore,
      rhythmScore: result.rhythmScore,
      fluencyScore: result.fluencyScore,
      completeScore: result.completeScore,
      level: result.level,
      targetCount: result.targetCount,
      alignmentMethod: result.alignmentMethod,
      startAnchorTargetIndex: result.startAnchorTargetIndex,
      startAnchorPlayedIndex: result.startAnchorPlayedIndex,
      leadingMissingCount: result.leadingMissingCount,
      leadingExtraCount: result.leadingExtraCount,
      alignmentOriginLocked: result.alignmentOriginLocked,
      tempoSampleCount: result.tempoSampleCount,
      details: result.details || [],
      chartStats: this.data.chartStats
    };
    const records = wx.getStorageSync('practiceRecords') || [];
    records.unshift(record);
    wx.setStorageSync('practiceRecords', records);
    this.setData({
      status: '已完成',
      statusClass: 'finished',
      isRecording: false,
      result
    }, () => this.drawPracticeChart());
    wx.showToast({ title: '评分完成', icon: 'success' });
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
    this.setData({ isStarting: true });
    api.restartPreparedPractice()
      .then((result) => {
        if (!result || result.ok === false) {
          throw new Error(api.messageText(result && result.message, '重新练习失败'));
        }
        this.applyPreparationResult(result);
        this.setData({
          status: '正在记录',
          statusClass: 'recording',
          isRecording: true,
          result: this.buildEmptyResult()
        }, () => this.drawPracticeChart());
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

  requestAiScore() {
    if (this.adviceRequestInFlight || this.data.isAdviceLoading) return false;
    this.adviceRequestInFlight = true;
    this.stopStatusPolling();
    this.setData({ isAdviceLoading: true });
    wx.showLoading({ title: 'AI 分析中' });
    return api.requestAiScore()
      .then((result) => {
        if (!result || result.ok === false) {
          throw new Error(api.messageText(result && result.message, 'AI 建议生成失败'));
        }
        const content = formatPracticeAdvice(result);
        wx.showModal({
          title: 'AI 练习建议',
          content,
          showCancel: false
        });
      })
      .catch((err) => wx.showModal({
        title: 'AI 建议未完成',
        content: `${api.errorMessage(err, 'AI 分析失败')}\n请确认设备页地址、ESP32 网络和 DEEPSEEK_API_KEY 配置。`,
        showCancel: false
      }))
      .finally(() => {
        this.adviceRequestInFlight = false;
        wx.hideLoading();
        this.setData({ isAdviceLoading: false }, () => {
          if (this.pageVisible) this.startStatusPolling();
        });
      });
  },

  formatPracticeAdvice
});
