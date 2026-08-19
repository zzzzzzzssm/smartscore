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

function normalizeAdviceState(value) {
  const state = String(value || 'none').toLowerCase();
  return [
    'none',
    'waiting_score',
    'running',
    'ready',
    'skipped_offline',
    'failed',
    'sync_missed'
  ].includes(state) ? state : 'none';
}

function adviceButtonText(state) {
  const normalized = normalizeAdviceState(state);
  if (normalized === 'running') return '建议生成中';
  if (normalized === 'ready') return '查看练习建议';
  if (['skipped_offline', 'failed', 'sync_missed'].includes(normalized)) {
    return '建议未生成';
  }
  return '练习建议';
}

function adviceStatusLabel(state) {
  const normalized = normalizeAdviceState(state);
  if (normalized === 'ready') return '有练习建议';
  if (['waiting_score', 'running'].includes(normalized)) return '建议生成中';
  if (['skipped_offline', 'failed', 'sync_missed'].includes(normalized)) {
    return '建议未生成';
  }
  return '暂无建议';
}

function updateRecordBySession(records, sessionId, patch) {
  const id = String(sessionId || '');
  let updated = false;
  const next = (Array.isArray(records) ? records : []).map((record) => {
    if (!id || record.practiceSessionId !== id) return record;
    updated = true;
    return Object.assign({}, record, patch || {});
  });
  return { records: next, updated };
}

function markMismatchedPending(records, currentSessionId) {
  const current = String(currentSessionId || '');
  if (!current) return { records, updated: false };
  let updated = false;
  const next = (Array.isArray(records) ? records : []).map((record) => {
    if (record.adviceStatus !== 'pending' || !record.practiceSessionId ||
        record.practiceSessionId === current) return record;
    updated = true;
    return Object.assign({}, record, {
      adviceStatus: 'sync_missed',
      adviceMessage: '设备已开始下一次练习，这条建议未完成同步'
    });
  });
  return { records: next, updated };
}

module.exports = {
  formatPracticeAdvice,
  normalizeAdviceState,
  adviceButtonText,
  adviceStatusLabel,
  updateRecordBySession,
  markMismatchedPending
};
