// CYD GPS web page: history of acceleration runs (POMIAR). Served by the CYD at /runs.js.
// The CYD appends every run to /runs/runs.csv (lib/accel_core/src/acc_hist.c) and keeps its speed
// trace in /runs/<id>.csv; the phone does the statistics and charts.
// The pure part (parseRunsCsv, summarize, parseTrace, plural) is tested in Node: test/host/test_runs.mjs.
(function (root) {
  'use strict';

  // ---------------------------------------------------------------- pure functions
  const METRICS = ['0–50 km/h', '0–100 km/h', '0–200 km/h', '100–200 km/h', '100 m', '1/4 mili'];

  const num = s => (s === undefined || s === '' ? null : Number(s));

  // runs.csv -> [{id, utc, t[6] (s or null), v100m, vquarter, vmax, dist, dur, grade, climb,
  // aborted, lowRate, rate}] in file order (oldest first). Unknown columns are ignored, missing
  // ones are null, so older or newer firmware still reads.
  function parseRunsCsv(text) {
    const lines = String(text || '').split(/\r?\n/).filter(l => l.trim() !== '');
    if (!lines.length) return [];
    const head = lines[0].split(',');
    const col = name => head.indexOf(name);
    const tcols = ['t0_50', 't0_100', 't0_200', 't100_200', 't100m', 'tquarter'].map(col);
    const out = [];
    for (const line of lines.slice(1)) {
      const f = line.split(',');
      const get = name => (col(name) >= 0 ? f[col(name)] : undefined);
      if (!get('id')) continue;
      out.push({
        id: get('id'),
        utc: get('utc') || '',
        t: tcols.map(c => (c >= 0 ? num(f[c]) : null)).map(v => (v !== null && v > 0 ? v : null)),
        v100m: num(get('v100m')), vquarter: num(get('vquarter')), vmax: num(get('vmax')),
        dist: num(get('dist_m')), dur: num(get('dur_s')),
        grade: num(get('grade_pct')), climb: num(get('climb_m')),
        aborted: get('aborted') === '1', lowRate: get('low_rate') === '1', rate: num(get('rate_hz')),
      });
    }
    return out;
  }

  // Same rule as acc_hist_counts(): interrupted and 1 Hz runs are listed, not counted.
  const counts = r => !r.aborted && !r.lowRate;

  // Per metric: {n, best, mean, sd (sample, null below 2), bestId}
  function summarize(runs) {
    return METRICS.map((_, i) => {
      const vals = runs.filter(r => counts(r) && r.t[i] !== null).map(r => ({ v: r.t[i], id: r.id }));
      const n = vals.length;
      if (!n) return { n: 0, best: null, mean: null, sd: null, bestId: null };
      const mean = vals.reduce((a, x) => a + x.v, 0) / n;
      const sd = n > 1 ? Math.sqrt(vals.reduce((a, x) => a + (x.v - mean) ** 2, 0) / (n - 1)) : null;
      const b = vals.reduce((a, x) => (x.v < a.v ? x : a));
      return { n, best: b.v, mean, sd, bestId: b.id };
    });
  }

  // trace CSV "t_s,kmh" -> [{t, v}]
  function parseTrace(text) {
    return String(text || '').split(/\r?\n/).slice(1).map(l => l.split(',').map(Number))
      .filter(a => a.length >= 2 && isFinite(a[0]) && isFinite(a[1])).map(([t, v]) => ({ t, v }));
  }

  // 1 pomiar, 2-4 pomiary, 5 pomiarów, 12-14 pomiarów, 22 pomiary
  function plural(n) {
    if (n === 1) return '1 pomiar';
    const d = n % 10, dd = n % 100;
    return n + (d >= 2 && d <= 4 && (dd < 12 || dd > 14) ? ' pomiary' : ' pomiarów');
  }

  // "z górki 2,3 %", "pod górę 1,1 %", "płasko" (under 0.5 %), "" (unknown)
  function gradeText(g) {
    if (g === null || g === undefined || !isFinite(g)) return '';
    if (Math.abs(g) < 0.5) return 'płasko';
    return (g > 0 ? 'pod górę ' : 'z górki ') + Math.abs(g).toFixed(1).replace('.', ',') + ' %';
  }

  if (typeof module === 'object' && module.exports) {
    module.exports = { parseRunsCsv, summarize, parseTrace, plural, gradeText, METRICS };
    return;
  }

  // ---------------------------------------------------------------- page
  const doc = root.document;
  const el = (tag, cls, text) => {
    const e = doc.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  };
  const SVG = 'http://www.w3.org/2000/svg';
  const svg = (tag, attrs) => {
    const e = doc.createElementNS(SVG, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  };
  const sec = v => (v === null || v === undefined ? '—' : v.toFixed(2).replace('.', ',') + ' s');
  // table cell that never wraps "3,91 s" over two lines; bold for the main value
  const cell = (text, bold) => {
    const td = el('td', '', text);
    td.style.whiteSpace = 'nowrap';
    if (bold) td.style.fontWeight = '600';
    return td;
  };
  const COLORS = ['#ff7a1a', '#4fc3f7'];

  // UTC "2026-11-24 15:30:12" -> phone's local time; runs without a GPS date show their number
  function when(r) {
    if (!r.utc) return 'pomiar ' + r.id.slice(1).replace(/^0+/, '');
    const d = new Date(r.utc.replace(' ', 'T') + 'Z');
    return d.toLocaleString('pl-PL', { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' });
  }

  let box, runs = [], picked = [], shownCount = 20, lastText = null;
  const traces = {};

  async function trace(id) {
    if (!traces[id]) {
      const r = await fetch('/api/run?id=' + encodeURIComponent(id));
      if (!r.ok) throw new Error(r.status);
      traces[id] = parseTrace(await r.text());
    }
    return traces[id];
  }

  function drawChart(host, series) {
    const W = 360, H = 190, l = 34, r = 8, t = 10, b = 24;
    const tmax = Math.max(1, ...series.map(s => s.pts.length ? s.pts[s.pts.length - 1].t : 0));
    const vmax = Math.max(20, ...series.map(s => Math.max(0, ...s.pts.map(p => p.v))));
    const step = vmax > 120 ? 50 : vmax > 60 ? 20 : 10, top = Math.ceil(vmax / step) * step;
    const tstep = tmax > 30 ? 10 : tmax > 12 ? 5 : tmax > 5 ? 2 : 1;
    const x = tt => l + tt / tmax * (W - l - r), y = v => t + (1 - v / top) * (H - t - b);
    const s = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart' });
    const text = (xx, yy, str, anchor) => {
      const e = svg('text', { x: xx, y: yy, 'text-anchor': anchor, fill: '#7f8a94', 'font-size': 10 });
      e.textContent = str;
      s.appendChild(e);
    };
    for (let v = 0; v <= top; v += step) {
      s.appendChild(svg('line', { x1: l, x2: W - r, y1: y(v), y2: y(v), stroke: '#2a3138' }));
      text(l - 4, y(v) + 4, v, 'end');
    }
    for (let tt = 0; tt <= tmax + 1e-9; tt += tstep) text(x(tt), H - 8, tt + ' s', 'middle');
    series.forEach((ser, k) => {
      const pts = ser.pts.map(p => x(p.t).toFixed(1) + ',' + y(p.v).toFixed(1)).join(' ');
      s.appendChild(svg('polyline', { points: pts, fill: 'none', stroke: COLORS[k], 'stroke-width': 2 }));
    });
    const legend = el('div', 'k');
    series.forEach((ser, k) => {
      const sw = el('span', '', '■ ');
      sw.style.color = COLORS[k];
      legend.append(sw, el('span', '', ser.label + (k + 1 < series.length ? '   ' : '')));
    });
    host.append(el('div', 'k', 'Prędkość [km/h] od startu'), s, legend);
  }

  async function showChart() {
    const host = box.querySelector('#runchart');
    host.textContent = '';
    if (!picked.length) {
      host.appendChild(el('small', '', 'Dotknij pomiaru, aby zobaczyć wykres; zaznacz dwa, aby je porównać.'));
      return;
    }
    host.appendChild(el('div', 'k', 'Wczytuję…'));
    try {
      const series = [];
      for (const id of picked) {
        const r = runs.find(x => x.id === id);
        series.push({ pts: await trace(id), label: r ? when(r) : id });
      }
      host.textContent = '';
      drawChart(host, series);
    } catch (e) {
      host.textContent = 'Brak przebiegu dla tego pomiaru.';
    }
  }

  function render() {
    box.textContent = '';
    if (!runs.length) {
      box.appendChild(el('small', '', 'Historia pomiarów: jeszcze pusta (zapis na karcie SD w CYD).'));
      return;
    }
    const counted = runs.filter(counts).length;
    box.appendChild(el('h3', '', 'Historia: ' + plural(runs.length)));
    if (counted < runs.length) {
      box.appendChild(el('small', '', 'Przerwane lub przy GPS 1 Hz: ' + (runs.length - counted) +
        ' (są na liście, nie wchodzą do statystyk).'));
    }

    // summary per metric
    const sum = summarize(runs);
    const st = el('table');
    st.innerHTML = '<tr><td class="k"></td><td class="k">pomiarów</td><td class="k">najlepszy</td>' +
      '<td class="k">średni</td><td class="k">rozrzut</td></tr>';
    METRICS.forEach((m, i) => {
      const s = sum[i];
      if (!s.n) return;
      const tr = el('tr');
      tr.append(el('td', '', m), el('td', '', String(s.n)), cell(sec(s.best), true), cell(sec(s.mean)),
        cell(s.sd === null ? '—' : '±' + s.sd.toFixed(2).replace('.', ',') + ' s'));
      st.appendChild(tr);
    });
    box.appendChild(st);
    box.appendChild(el('small', '', 'Rozrzut: odchylenie standardowe — im mniejsze, tym powtarzalniejsze starty. ' +
      'Liczone bez pomiarów przerwanych i przy GPS 1 Hz.'));

    // list, newest first
    const list = el('table');
    list.innerHTML = '<tr><td class="k"></td><td class="k">kiedy</td><td class="k">0–100</td>' +
      '<td class="k">1/4 mili</td><td class="k">Vmax</td><td class="k">droga</td></tr>';
    const newest = runs.slice().reverse();
    newest.slice(0, shownCount).forEach(r => {
      const tr = el('tr');
      tr.style.cursor = 'pointer';
      const cb = el('input');
      cb.type = 'checkbox';
      cb.style.width = 'auto';
      cb.checked = picked.includes(r.id);
      cb.onclick = e => {
        e.stopPropagation();
        picked = cb.checked ? picked.concat(r.id).slice(-2) : picked.filter(x => x !== r.id);
        render();
        showChart();
      };
      const c0 = el('td');
      c0.appendChild(cb);
      const flags = r.aborted ? ' (przerwany)' : r.lowRate ? ' (1 Hz)' : '';
      const road = el('td', 'k', gradeText(r.grade));
      road.style.fontSize = '13px';
      tr.append(c0, el('td', '', when(r) + flags), cell(sec(r.t[1]), true), cell(sec(r.t[5])),
        cell(r.vmax === null ? '—' : Math.round(r.vmax) + ''), road);
      tr.onclick = () => {
        picked = [r.id];
        render();
        showChart();
      };
      list.appendChild(tr);
    });
    box.appendChild(list);
    if (newest.length > shownCount) {
      const more = el('button', '', 'Pokaż starsze (' + (newest.length - shownCount) + ')');
      more.onclick = () => { shownCount += 20; render(); showChart(); };
      box.appendChild(more);
    }
    const chart = el('div');
    chart.id = 'runchart';
    box.appendChild(chart);

    const act = el('p');
    const exp = el('a', 'btn', 'Eksport CSV');
    exp.href = '/api/runs?dl=1';
    const clr = el('button', '', 'Usuń historię');
    clr.onclick = async () => {
      if (!confirm('Usunąć całą historię pomiarów i ich wykresy z karty? Najlepsze wyniki zostaną.')) return;
      await fetch('/api/runs/clear', { method: 'POST' });
      picked = [];
      setTimeout(refresh, 1500);
    };
    act.append(exp, ' ', clr);
    box.appendChild(act);
  }

  async function refresh() {
    try {
      const r = await fetch('/api/runs');
      const text = await r.text();
      if (text === lastText) return;  // nothing new: keep the chart as it is
      lastText = text;
      runs = parseRunsCsv(text);
      picked = picked.filter(id => runs.some(x => x.id === id));
      render();
      showChart();
    } catch (e) {
      /* offline for a moment: try again later */
    }
  }

  function init(host) {
    box = host;
    refresh();
    setInterval(refresh, 20000);
  }

  root.CydRuns = { init };
})(typeof window !== 'undefined' ? window : globalThis);
