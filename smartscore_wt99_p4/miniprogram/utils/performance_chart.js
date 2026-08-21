const MIN_NOTE_DURATION = 0.08;

function finiteNumber(value) {
  if (value === null || value === undefined) return null;
  if (typeof value === 'string' && value.trim() === '') return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function positiveInteger(value) {
  const number = finiteNumber(value);
  return number !== null && Number.isInteger(number) && number > 0 ? number : 0;
}

function validMidi(value) {
  const number = finiteNumber(value);
  return number !== null && Number.isInteger(number) && number >= 0 && number <= 127
    ? number
    : null;
}

function nonNegativeNumber(value) {
  const number = finiteNumber(value);
  return number !== null && number >= 0 ? number : null;
}

function normalizedDuration(value) {
  const number = finiteNumber(value);
  return number !== null && number >= MIN_NOTE_DURATION ? number : MIN_NOTE_DURATION;
}

function resultNumber(result, camelName, snakeName) {
  const camelValue = finiteNumber(result && result[camelName]);
  if (camelValue !== null) return camelValue;
  return finiteNumber(result && result[snakeName]);
}

function targetCountOf(result) {
  const camelCount = positiveInteger(result && result.targetCount);
  return camelCount || positiveInteger(result && result.target_count);
}

function normalizeTargetNotes(notes) {
  return (Array.isArray(notes) ? notes : [])
    .map((note) => {
      const midi = validMidi(note && note.midi);
      const start = nonNegativeNumber(note && note.start);
      if (midi === null || start === null) return null;
      return {
        midi,
        start,
        duration: normalizedDuration(note && note.duration)
      };
    })
    .filter(Boolean)
    .sort((left, right) => left.start - right.start || left.midi - right.midi);
}

function getMatchedDetails(result) {
  const details = result && Array.isArray(result.details) ? result.details : [];
  return details
    .filter((item) => {
      const refIndex = positiveInteger(item && item.ref_index);
      const targetMidi = validMidi(item && item.target_midi);
      const playedMidi = validMidi(item && item.played_midi);
      const targetStart = nonNegativeNumber(item && item.target_start);
      const playedStart = nonNegativeNumber(item && item.played_start);
      return item &&
        item.result !== 'uncertain' &&
        item.result !== 'extra' &&
        item.result !== 'retry' &&
        refIndex > 0 &&
        targetMidi !== null &&
        playedMidi !== null &&
        targetStart !== null &&
        playedStart !== null;
    })
    .slice()
    .sort((left, right) => Number(left.ref_index) - Number(right.ref_index));
}

function buildDetailTargets(result) {
  const details = result && Array.isArray(result.details) ? result.details : [];
  const targets = new Map();
  details.forEach((item) => {
    const refIndex = positiveInteger(item && item.ref_index);
    const midi = validMidi(item && item.target_midi);
    const start = nonNegativeNumber(item && item.target_start);
    if (!refIndex || midi === null || start === null || targets.has(refIndex)) return;
    targets.set(refIndex, {
      refIndex,
      midi,
      start,
      duration: normalizedDuration(item && item.target_duration)
    });
  });
  return Array.from(targets.values())
    .sort((left, right) => left.refIndex - right.refIndex);
}

function detailTargetsAreComplete(detailTargets, targetCount) {
  if (!targetCount || detailTargets.length !== targetCount) return false;
  return detailTargets.every((note, index) => note.refIndex === index + 1);
}

function compactTargetNotes(notes) {
  return normalizeTargetNotes(notes).map((note) => ({
    midi: note.midi,
    start: note.start,
    duration: note.duration
  }));
}

function selectTargetNotes(result, targetNotes) {
  const detailTargets = buildDetailTargets(result);
  const targetCount = targetCountOf(result);
  if (detailTargetsAreComplete(detailTargets, targetCount)) {
    return {
      notes: compactTargetNotes(detailTargets),
      partial: false,
      source: 'details'
    };
  }

  const snapshot = compactTargetNotes(targetNotes);
  if (snapshot.length) {
    return {
      notes: snapshot,
      partial: false,
      source: 'snapshot'
    };
  }

  if (detailTargets.length) {
    return {
      notes: compactTargetNotes(detailTargets),
      partial: true,
      source: 'details'
    };
  }

  return { notes: [], partial: false, source: 'empty' };
}

function estimateVisualOffset(matched) {
  if (matched.some((item) => nonNegativeNumber(item.aligned_played_start) !== null)) {
    return 0;
  }
  const offsets = matched
    .map((item) => {
      const playedStart = nonNegativeNumber(item.played_start);
      const targetStart = nonNegativeNumber(item.target_start);
      return playedStart !== null && targetStart !== null
        ? playedStart - targetStart
        : null;
    })
    .filter((value) => value !== null)
    .sort((left, right) => left - right);
  if (!offsets.length) return 0;
  return offsets[Math.floor(offsets.length / 2)];
}

function alignedPlayedStart(item, result, legacyOffset) {
  const aligned = nonNegativeNumber(item && item.aligned_played_start);
  if (aligned !== null) return aligned;

  const raw = nonNegativeNumber(item && item.played_start);
  const offset = resultNumber(result, 'startOffset', 'start_offset');
  const tempo = resultNumber(result, 'tempoScale', 'tempo_scale');
  if (raw !== null && offset !== null && tempo !== null && tempo > 0) {
    return (raw - offset) / tempo;
  }
  return raw === null ? 0 : raw - Number(legacyOffset || 0);
}

function average(values) {
  if (!values.length) return null;
  return values.reduce((sum, value) => sum + value, 0) / values.length;
}

function calculateStats(result, matched, targetNotes) {
  const targetCount = targetCountOf(result);
  const denominator = targetCount || targetNotes.length || matched.length;
  if (!matched.length) {
    return {
      avgPitch: '-',
      maxPitch: '-',
      avgRhythm: '-',
      matched: `0 / ${denominator || '-'}`
    };
  }

  const pitchErrors = matched.map((item) => (
    Math.abs(Number(item.played_midi) - Number(item.target_midi))
  ));
  const rhythmErrors = matched
    .map((item) => finiteNumber(item.time_error))
    .filter((value) => value !== null)
    .map((value) => Math.abs(value));
  const avgPitch = average(pitchErrors);
  const avgRhythm = average(rhythmErrors);
  const maxPitch = Math.max.apply(null, pitchErrors);

  return {
    avgPitch: `${avgPitch.toFixed(2)} 半音`,
    maxPitch: `${maxPitch.toFixed(2)} 半音`,
    avgRhythm: avgRhythm === null ? '-' : `${avgRhythm.toFixed(2)}s`,
    matched: `${matched.length} / ${denominator || '-'}`
  };
}

function chartHint(result, matched, selection, context) {
  const history = context === 'history';
  if (!selection.notes.length && !matched.length) {
    return history
      ? '这条记录没有可展示的演奏数据。'
      : '结束练习后会显示标准谱、采集演奏和音高误差。';
  }
  if (history && selection.partial) {
    return '这条历史记录仅保留了可展示的演奏片段。';
  }
  if (!matched.length) {
    return history
      ? '这条记录只保存了标准谱，暂无可展示的采集演奏。'
      : '已载入标准谱；结束评分后会叠加采集演奏和音高误差。';
  }
  if (result && (result.alignmentOriginLocked || result.alignment_origin_locked === true)) {
    return '已锁定乐谱开头和起奏时间原点；等待起奏不会把演奏吸附到后面的重复乐句。';
  }
  return 'MIDI 已按音符顺序和演奏速度完成时间归一化，起奏等待不计分。';
}

function buildChartModel(result, targetNotes, context) {
  const safeResult = result || {};
  const matched = getMatchedDetails(safeResult);
  const selection = selectTargetNotes(safeResult, targetNotes);
  const visualOffset = estimateVisualOffset(matched);
  return {
    matched,
    targetNotes: selection.notes,
    visualOffset,
    chartEmpty: !selection.notes.length && !matched.length,
    chartHint: chartHint(safeResult, matched, selection, context),
    chartStats: calculateStats(safeResult, matched, selection.notes),
    partial: selection.partial,
    source: selection.source
  };
}

function midiName(midi) {
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  const value = Math.round(Number(midi));
  return `${names[((value % 12) + 12) % 12]}${Math.floor(value / 12) - 1}`;
}

module.exports = {
  alignedPlayedStart,
  buildChartModel,
  buildDetailTargets,
  calculateStats,
  compactTargetNotes,
  detailTargetsAreComplete,
  finiteNumber,
  getMatchedDetails,
  midiName,
  normalizeTargetNotes,
  selectTargetNotes,
  targetCountOf
};
