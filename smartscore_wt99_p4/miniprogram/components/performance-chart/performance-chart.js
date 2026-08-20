const chart = require('../../utils/performance_chart');

function playedDuration(item) {
  const duration = chart.finiteNumber(item && item.played_duration);
  return duration === null ? 0.25 : Math.max(0.08, duration);
}

Component({
  properties: {
    result: {
      type: Object,
      value: null
    },
    targetNotes: {
      type: Array,
      value: []
    },
    context: {
      type: String,
      value: 'practice'
    }
  },

  data: {
    chartEmpty: true,
    chartHint: '结束练习后会显示标准谱、采集演奏和音高误差。',
    chartStats: {
      avgPitch: '-',
      maxPitch: '-',
      avgRhythm: '-',
      matched: '0 / -'
    }
  },

  observers: {
    'result,targetNotes,context': function handleChartInput() {
      this.scheduleDraw();
    }
  },

  lifetimes: {
    attached() {
      this.chartAttached = true;
      this.scheduleDraw();
    },

    detached() {
      this.chartAttached = false;
      this.drawRevision = (this.drawRevision || 0) + 1;
    }
  },

  methods: {
    scheduleDraw() {
      const revision = (this.drawRevision || 0) + 1;
      this.drawRevision = revision;
      if (!this.chartAttached) return;
      const run = () => {
        if (!this.chartAttached || revision !== this.drawRevision) return;
        this.drawChart();
      };
      if (wx.nextTick) wx.nextTick(run);
      else setTimeout(run, 0);
    },

    drawChart() {
      const revision = this.drawRevision;
      const result = this.properties.result || {};
      const model = chart.buildChartModel(
        result,
        this.properties.targetNotes,
        this.properties.context
      );
      this.setData({
        chartEmpty: model.chartEmpty,
        chartHint: model.chartHint,
        chartStats: model.chartStats
      }, () => {
        if (this.chartAttached && revision === this.drawRevision) {
          this.drawCanvas(result, model, revision);
        }
      });
    },

    drawCanvas(result, model, revision) {
      const query = this.createSelectorQuery();
      query.select('#performanceChart').fields({ node: true, size: true }, (canvasInfo) => {
        if (!canvasInfo || !canvasInfo.node || !canvasInfo.width ||
            !canvasInfo.height || !this.chartAttached ||
            revision !== this.drawRevision) return;
        const canvas = canvasInfo.node;
        const width = canvasInfo.width;
        const height = canvasInfo.height;
        const systemInfo = wx.getSystemInfoSync();
        const pixelRatio = Math.max(1, Number(systemInfo.pixelRatio) || 1);
        canvas.width = Math.round(width * pixelRatio);
        canvas.height = Math.round(height * pixelRatio);
        const ctx = canvas.getContext('2d');
        ctx.scale(pixelRatio, pixelRatio);
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
        const targetNotes = model.targetNotes;
        const matched = model.matched;

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

        const targetEnd = targetNotes.reduce((max, note) => (
          Math.max(max, Number(note.start) + Number(note.duration))
        ), 0);
        const playedEnd = matched.reduce((max, item) => {
          const start = chart.alignedPlayedStart(item, result, model.visualOffset);
          return Math.max(max, start + playedDuration(item));
        }, 0);
        const timeMax = Math.max(2, targetEnd, playedEnd);
        const xOf = (time) => (
          chartLeft + (Math.max(0, Number(time || 0)) / timeMax) * chartWidth
        );
        const yOf = (midi) => (
          pitchBottom - ((Number(midi) - midiMin) / midiRange) * pitchHeight
        );
        const yErr = (error) => {
          const clamped = Math.max(-3, Math.min(3, Number(error || 0)));
          return errorTop + ((3 - clamped) / 6) * (errorBottom - errorTop);
        };

        ctx.fillStyle = '#f8fbfd';
        ctx.fillRect(0, 0, width, height);
        ctx.font = '10px sans-serif';
        ctx.fillStyle = '#667085';
        ctx.strokeStyle = '#dfe8ef';
        ctx.lineWidth = 1;
        for (let index = 0; index <= 4; index += 1) {
          const y = pitchTop + (pitchHeight / 4) * index;
          ctx.beginPath();
          ctx.moveTo(chartLeft, y);
          ctx.lineTo(chartRight, y);
          ctx.stroke();
          const midi = midiMax - (midiRange / 4) * index;
          ctx.fillText(chart.midiName(midi), 4, y + 3);
        }
        for (let index = 0; index <= 4; index += 1) {
          const x = chartLeft + (chartWidth / 4) * index;
          ctx.beginPath();
          ctx.moveTo(x, pitchTop);
          ctx.lineTo(x, errorBottom);
          ctx.stroke();
          ctx.fillText(`${Math.round((timeMax / 4) * index)}s`, x - 8, height - 10);
        }

        if (targetNotes.length) {
          ctx.strokeStyle = '#2563eb';
          ctx.lineWidth = 3;
          ctx.lineCap = 'round';
          targetNotes.forEach((note) => {
            const y = yOf(note.midi);
            ctx.beginPath();
            ctx.moveTo(xOf(note.start), y);
            ctx.lineTo(xOf(Number(note.start) + Number(note.duration)), y);
            ctx.stroke();
          });
        }

        if (matched.length) {
          ctx.strokeStyle = '#0f766e';
          ctx.fillStyle = '#0f766e';
          ctx.lineWidth = 2;
          ctx.beginPath();
          matched.forEach((item, index) => {
            const x = xOf(chart.alignedPlayedStart(item, result, model.visualOffset));
            const y = yOf(item.played_midi);
            if (index === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
          });
          ctx.stroke();
          matched.forEach((item) => {
            const x = xOf(chart.alignedPlayedStart(item, result, model.visualOffset));
            const y = yOf(item.played_midi);
            ctx.beginPath();
            ctx.arc(x, y, 3, 0, Math.PI * 2);
            ctx.fill();
          });
        }

        ctx.strokeStyle = '#94a3b8';
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(chartLeft, yErr(0));
        ctx.lineTo(chartRight, yErr(0));
        ctx.stroke();
        ctx.fillStyle = '#667085';
        ctx.fillText('+3', 8, errorTop + 6);
        ctx.fillText('0', 18, yErr(0) + 3);
        ctx.fillText('-3', 8, errorBottom + 3);

        if (matched.length) {
          ctx.strokeStyle = '#dc2626';
          ctx.lineWidth = 2;
          ctx.beginPath();
          matched.forEach((item, index) => {
            const error = Number(item.played_midi) - Number(item.target_midi);
            const x = xOf(Number(item.target_start));
            const y = yErr(error);
            if (index === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
          });
          ctx.stroke();
        }
      }).exec();
    }
  }
});
