const { formatTime } = require('./util');
const performanceChart = require('./performance_chart');

const PENDING_CONTEXT_KEY = 'pendingPracticeRecord';

function normalizeResult(result) {
  if (!result || result.ok === false) {
    throw new Error('评分失败');
  }

  const scorable = result.scorable !== false;
  const referenceOnly = result.score_status === 'reference';
  const inputConfidence = Number(result.input_confidence);
  const startOffset = performanceChart.finiteNumber(result.start_offset);
  const tempoScaleValue = performanceChart.finiteNumber(result.tempo_scale);
  const tempoScale = tempoScaleValue !== null && tempoScaleValue > 0
    ? tempoScaleValue
    : null;
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
    startOffset,
    tempoScale,
    alignmentMethod: result.alignment_method || '',
    startAnchorTargetIndex: Number(result.start_anchor_target_index || 0),
    startAnchorPlayedIndex: Number(result.start_anchor_played_index || 0),
    leadingMissingCount: Number(result.leading_missing_count || 0),
    leadingExtraCount: Number(result.leading_extra_count || 0),
    alignmentOriginLocked: result.alignment_origin_locked === true,
    tempoSampleCount: Number(result.tempo_sample_count || 0),
    details: Array.isArray(result.details) ? result.details : [],
    practiceSessionId: String(result.practice_session_id || '')
  };
}

function buildRecord(result, context) {
  const safeContext = context || {};
  const frozenTargetNotes = performanceChart.compactTargetNotes(
    safeContext.targetNotes
  );
  const targetNotes = frozenTargetNotes.length
    ? frozenTargetNotes
    : performanceChart.selectTargetNotes(result, []).notes;
  const chartModel = performanceChart.buildChartModel(
    result, targetNotes, 'history'
  );
  const record = {
    id: `record_${Date.now()}`,
    scoreTitle: String(safeContext.scoreTitle || '自动完成练习'),
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
    targetNotes: performanceChart.compactTargetNotes(targetNotes),
    chartStats: chartModel.chartStats,
    practiceSessionId: result.practiceSessionId,
    adviceStatus: result.practiceSessionId ? 'pending' : 'sync_missed',
    advice: null,
    adviceText: '',
    adviceMessage: result.practiceSessionId
      ? '练习建议正在后台生成'
      : '设备未返回练习会话编号，建议无法同步'
  };
  if (performanceChart.finiteNumber(result.startOffset) !== null) {
    record.startOffset = Number(result.startOffset);
  }
  if (performanceChart.finiteNumber(result.tempoScale) !== null &&
      Number(result.tempoScale) > 0) {
    record.tempoScale = Number(result.tempoScale);
  }
  return record;
}

function findBySession(records, sessionId) {
  const id = String(sessionId || '');
  if (!id) return null;
  return (Array.isArray(records) ? records : []).find(
    (record) => String(record && record.practiceSessionId || '') === id
  ) || null;
}

function rememberPendingContext(context) {
  const safeContext = context || {};
  try {
    wx.setStorageSync(PENDING_CONTEXT_KEY, {
      scoreTitle: String(safeContext.scoreTitle || ''),
      targetNotes: performanceChart.compactTargetNotes(safeContext.targetNotes),
      startedAt: Date.now()
    });
    return true;
  } catch (error) {
    return false;
  }
}

function readPendingContext() {
  try {
    const context = wx.getStorageSync(PENDING_CONTEXT_KEY);
    return context && typeof context === 'object' ? context : {};
  } catch (error) {
    return {};
  }
}

function clearPendingContext() {
  try {
    wx.removeStorageSync(PENDING_CONTEXT_KEY);
  } catch (error) {
    return false;
  }
  return true;
}

module.exports = {
  buildRecord,
  clearPendingContext,
  findBySession,
  normalizeResult,
  readPendingContext,
  rememberPendingContext
};
