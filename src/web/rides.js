// CYD GPS web page: "Przejazdy" (ride list, map, speed chart). Served by the CYD at /rides.js.
// All the maths runs in the phone; the CYD only streams the GPX files.
// The pure part (parseGpx, rideStats) is tested in Node: test/host/test_rides.mjs.
(function (root) {
  'use strict';

  // ---------------------------------------------------------------- pure functions
  const MOVE_GAP_S = 25;     // longer between two points = a stop (no points are logged while standing)
  const MEDIAN_N = 5;        // max speed of old files (no Doppler speed): median of this many steps
  const ELE_HYST_M = 5;      // climb counted only once the (smoothed) altitude rose this much
  const ELE_SMOOTH = 5;      // moving average over this many points

  // Same formula as odo_dist_m() in lib/odo_core.
  function dist(lat1, lon1, lat2, lon2) {
    const r = 6371000, k = Math.PI / 180;
    const dp = (lat2 - lat1) * k, dl = (lon2 - lon1) * k;
    let a = Math.sin(dp / 2) ** 2 + Math.cos(lat1 * k) * Math.cos(lat2 * k) * Math.sin(dl / 2) ** 2;
    if (a > 1) a = 1;
    return 2 * r * Math.asin(Math.sqrt(a));
  }

  // Parses the GPX the CYD writes (src/gpx_log.cpp). Returns [[{lat, lon, ele, t, v}]] per segment;
  // t in ms (UTC), ele null when missing, v in km/h or null (files from before v1.0.9).
  function parseGpx(text) {
    const segs = [];
    const segRe = /<trkseg>([\s\S]*?)(?=<\/trkseg>|$)/g;
    const ptRe = /<trkpt\s+lat="([-\d.]+)"\s+lon="([-\d.]+)"\s*>([\s\S]*?)<\/trkpt>/g;
    let s;
    while ((s = segRe.exec(text))) {
      const pts = [];
      let p;
      ptRe.lastIndex = 0;
      while ((p = ptRe.exec(s[1]))) {
        const body = p[3];
        const time = /<time>([^<]+)<\/time>/.exec(body);
        if (!time) continue;
        const ele = /<ele>([-\d.]+)<\/ele>/.exec(body);
        const spd = /<gpxtpx:speed>([-\d.]+)<\/gpxtpx:speed>/.exec(body);
        pts.push({
          lat: +p[1], lon: +p[2], t: Date.parse(time[1]),
          ele: ele ? +ele[1] : null,
          v: spd ? +spd[1] * 3.6 : null,
        });
      }
      if (pts.length) segs.push(pts);
    }
    return segs;
  }

  function climb(segs) {
    let gain = 0;
    for (const pts of segs) {
      const e = pts.map(p => p.ele).filter(x => x !== null);
      if (e.length < 2) continue;
      // centred moving average; at the ends the edge value is repeated, so noise at the start or
      // end does not count as a climb and a steady climb keeps (almost) its full height
      const h = ELE_SMOOTH >> 1;
      const at = j => e[Math.min(e.length - 1, Math.max(0, j))];
      const sm = e.map((_, i) => {
        let sum = 0;
        for (let j = i - h; j <= i + h; j++) sum += at(j);
        return sum / (2 * h + 1);
      });
      let ref = sm[0];
      for (const x of sm) {
        if (x >= ref + ELE_HYST_M) { gain += x - ref; ref = x; }
        else if (x <= ref - ELE_HYST_M) { ref = x; }
      }
    }
    return gain;
  }

  // Max speed of old files (no Doppler speed): per-step speeds through a median of 5, so a position
  // jump (out and back = 2 spiky steps) never becomes the maximum.
  function medianMax(pts) {
    const v = [];
    for (let i = 1; i < pts.length; i++) {
      const dt = (pts[i].t - pts[i - 1].t) / 1000;
      if (dt > 0 && dt <= MOVE_GAP_S) v.push(dist(pts[i - 1].lat, pts[i - 1].lon, pts[i].lat, pts[i].lon) / dt * 3.6);
    }
    let best = 0;
    for (let i = 0; i + MEDIAN_N <= v.length; i++) {
      const w = v.slice(i, i + MEDIAN_N).sort((a, b) => a - b);
      best = Math.max(best, w[MEDIAN_N >> 1]);
    }
    return best;
  }

  function rideStats(segs) {
    const st = {
      points: 0, segments: segs.length, distM: 0, movingS: 0, totalS: 0, avgKmh: 0, maxKmh: 0,
      climbM: 0, hasSpeed: false, start: null, end: null,
      bbox: null, series: [],   // series: [{t, v}] for the chart
    };
    let minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    for (const pts of segs) {
      st.points += pts.length;
      for (let i = 0; i < pts.length; i++) {
        const p = pts[i];
        minLat = Math.min(minLat, p.lat); maxLat = Math.max(maxLat, p.lat);
        minLon = Math.min(minLon, p.lon); maxLon = Math.max(maxLon, p.lon);
        if (st.start === null || p.t < st.start) st.start = p.t;
        if (st.end === null || p.t > st.end) st.end = p.t;
        if (p.v !== null) { st.hasSpeed = true; st.maxKmh = Math.max(st.maxKmh, p.v); }
        if (i === 0) { if (p.v !== null) st.series.push({ t: p.t, v: p.v }); continue; }
        const q = pts[i - 1];
        const d = dist(q.lat, q.lon, p.lat, p.lon);
        const dt = (p.t - q.t) / 1000;
        st.distM += d;
        if (dt > 0 && dt <= MOVE_GAP_S) st.movingS += dt;
        else if (dt > MOVE_GAP_S) st.series.push({ t: q.t + 1000, v: 0 }, { t: p.t - 1000, v: 0 });  // stop
        st.series.push({ t: p.t, v: p.v !== null ? p.v : (dt > 0 ? d / dt * 3.6 : 0) });
      }
    }
    if (!st.hasSpeed) {
      for (const pts of segs) st.maxKmh = Math.max(st.maxKmh, medianMax(pts));
      // chart of an old file: same median of 5, so a position jump is not drawn as a speed spike
      const raw = st.series.map(p => p.v), h = MEDIAN_N >> 1;
      st.series = st.series.map((p, i) => {
        const w = raw.slice(Math.max(0, i - h), i + h + 1).sort((a, b) => a - b);
        return { t: p.t, v: w[w.length >> 1] };
      });
    }
    if (st.points) {
      st.totalS = (st.end - st.start) / 1000;
      st.bbox = [minLat, minLon, maxLat, maxLon];
    }
    st.avgKmh = st.movingS > 0 ? st.distM / st.movingS * 3.6 : 0;
    st.climbM = climb(segs);
    return st;
  }

  if (typeof module === 'object' && module.exports) {
    module.exports = { dist, parseGpx, rideStats };
    return;
  }

  // ---------------------------------------------------------------- page
  const LEAFLET_JS = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js';
  const LEAFLET_JS_SRI = 'sha512-puJW3E/qXDqYp9IfhAI54BJEaWIfloJ7JWs7OeD5i6ruC9JZL1gERT1wjtwXFlh7CjE7ZJ+/vcRZRkIYIb6p4g==';
  const LEAFLET_CSS = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css';
  const LEAFLET_CSS_SRI = 'sha512-h9FcoyWjHcOcmEVkxOfTLnmZFWIH0iZhZT1H2TbOq55xssQGEJHEaIm+PgoUaZbRvQTNTluNOEfb1ZRy6D3BOw==';
  const CACHE = 'cydride1:';

  const doc = root.document;
  const $ = id => doc.getElementById(id);
  const mem = new Map();  // name:size -> {stats, segs}
  let files = [];
  let leaflet = null;     // Promise<boolean>

  const nf = d => new Intl.NumberFormat('pl-PL', { maximumFractionDigits: d, minimumFractionDigits: d });
  const km = m => m < 1000 ? Math.round(m) + ' m' : nf(m < 10000 ? 2 : 1).format(m / 1000) + ' km';
  const kmh = v => nf(1).format(v) + ' km/h';
  const dur = s => {
    s = Math.round(s);
    if (s < 60) return s + ' s';
    const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
    return h ? h + ' h ' + String(m).padStart(2, '0') + ' min' : m + ' min';
  };
  // "20261123-132830.gpx" (UTC) -> Date
  const fileDate = n => new Date(Date.UTC(+n.slice(0, 4), +n.slice(4, 6) - 1, +n.slice(6, 8),
                                          +n.slice(9, 11), +n.slice(11, 13), +n.slice(13, 15)));
  const when = n => fileDate(n).toLocaleString('pl-PL', { weekday: 'short', day: 'numeric', month: 'short',
                                                         hour: '2-digit', minute: '2-digit' });
  const el = (tag, cls, text) => {
    const e = doc.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  };

  function store(key, val) {
    try { root.localStorage.setItem(CACHE + key, JSON.stringify(val)); } catch (e) { /* private mode */ }
  }
  function load(key) {
    try { return JSON.parse(root.localStorage.getItem(CACHE + key)); } catch (e) { return null; }
  }

  async function ride(f) {
    const key = f.name + ':' + f.size;
    if (mem.has(key)) return mem.get(key);
    const r = await fetch('/gpx/' + encodeURIComponent(f.name));
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const segs = parseGpx(await r.text());
    const val = { stats: rideStats(segs), segs };
    for (const k of mem.keys()) if (k.startsWith(f.name + ':')) mem.delete(k);  // older size of a growing file
    mem.set(key, val);
    if (!f.active) {
      const s = Object.assign({}, val.stats);
      delete s.series;
      store(key, s);
    }
    return val;
  }

  function summary(st) {
    if (!st.points) return 'brak punktów';
    const parts = [km(st.distM), dur(st.movingS)];
    if (st.movingS > 0) parts.push('śr. ' + kmh(st.avgKmh));
    if (st.maxKmh > 0) parts.push('maks. ' + kmh(st.maxKmh));
    if (st.climbM >= 1) parts.push('↑ ' + Math.round(st.climbM) + ' m');
    return parts.join(' · ');
  }

  async function refreshList() {
    const box = $('rides');
    let d;
    try {
      d = await (await fetch('/api/files')).json();
    } catch (e) {
      return;
    }
    files = d.files.sort((a, b) => b.name.localeCompare(a.name));
    box.textContent = '';
    if (!files.length) {
      box.appendChild(el('div', 'k', d.sd ? 'Brak przejazdów. Trasa zapisuje się sama, gdy jest dobry fix.' : 'Brak karty SD'));
      return;
    }
    const todo = [];
    for (const f of files) {
      const row = el('a', 'ride');
      row.href = '#r=' + f.name;
      const top = el('div', 'rtop');
      top.appendChild(el('span', 'rwhen', when(f.name)));
      if (f.active) top.appendChild(el('span', 'badge', 'w trakcie'));
      const sub = el('div', 'k');
      row.append(top, sub);
      box.appendChild(row);
      const cached = !f.active && load(f.name + ':' + f.size);
      const inMem = mem.get(f.name + ':' + f.size);
      if (inMem) sub.textContent = summary(inMem.stats);
      else if (cached) sub.textContent = summary(cached);
      else { sub.textContent = 'liczę…'; todo.push([f, sub]); }
    }
    for (const [f, sub] of todo) {   // one at a time: the CYD serves one request at a time
      try { sub.textContent = summary((await ride(f)).stats); } catch (e) { sub.textContent = 'błąd odczytu'; }
    }
  }

  function loadLeaflet() {
    if (leaflet) return leaflet;
    leaflet = new Promise(resolve => {
      if (root.L) return resolve(true);
      const css = doc.createElement('link');
      css.rel = 'stylesheet'; css.href = LEAFLET_CSS; css.integrity = LEAFLET_CSS_SRI; css.crossOrigin = 'anonymous';
      doc.head.appendChild(css);
      const js = doc.createElement('script');
      js.src = LEAFLET_JS; js.integrity = LEAFLET_JS_SRI; js.crossOrigin = 'anonymous';
      const timer = setTimeout(() => resolve(false), 4000);  // no internet (CYD hotspot)
      js.onload = () => { clearTimeout(timer); resolve(!!root.L); };
      js.onerror = () => { clearTimeout(timer); resolve(false); };
      doc.head.appendChild(js);
    });
    leaflet.then(ok => { if (!ok) leaflet = null; });  // try again next time
    return leaflet;
  }

  const SVG = 'http://www.w3.org/2000/svg';
  const svg = (tag, attrs) => {
    const e = doc.createElementNS(SVG, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    return e;
  };

  // Offline map: the track alone, equirectangular projection, with a scale bar.
  function drawTrack(box, segs, st) {
    const W = 360, H = 260, pad = 16;
    const [la0, lo0, la1, lo1] = st.bbox;
    const kx = Math.cos((la0 + la1) / 2 * Math.PI / 180) * 111320, ky = 110540;
    const wm = Math.max((lo1 - lo0) * kx, 1), hm = Math.max((la1 - la0) * ky, 1);
    const sc = Math.min((W - 2 * pad) / wm, (H - 2 * pad - 20) / hm);
    const ox = (W - wm * sc) / 2, oy = (H - 20 - hm * sc) / 2;
    const xy = p => [ox + (p.lon - lo0) * kx * sc, oy + (la1 - p.lat) * ky * sc];
    const s = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'trk' });
    for (const pts of segs) {
      s.appendChild(svg('polyline', { points: pts.map(p => xy(p).map(v => v.toFixed(1)).join(',')).join(' '),
                                      fill: 'none', stroke: '#ff7a1a', 'stroke-width': 3,
                                      'stroke-linejoin': 'round', 'stroke-linecap': 'round' }));
    }
    const first = segs[0][0], lastSeg = segs[segs.length - 1], last = lastSeg[lastSeg.length - 1];
    for (const [p, c] of [[first, '#4cd97b'], [last, '#ff5a5a']]) {
      const [x, y] = xy(p);
      s.appendChild(svg('circle', { cx: x, cy: y, r: 6, fill: c, stroke: '#101418', 'stroke-width': 2 }));
    }
    // scale bar: 1/2/5 x 10^n metres, about a quarter of the width
    const target = (W / 4) / sc, pow = 10 ** Math.floor(Math.log10(target));
    const len = [1, 2, 5, 10].map(m => m * pow).filter(m => m <= target).pop() || pow;
    const y = H - 10;
    s.appendChild(svg('line', { x1: pad, y1: y, x2: pad + len * sc, y2: y, stroke: '#e8ecef', 'stroke-width': 2 }));
    const t = svg('text', { x: pad + len * sc + 6, y: y + 4, fill: '#9aa4ae', 'font-size': 12 });
    t.textContent = km(len);
    s.appendChild(t);
    box.appendChild(s);
    box.appendChild(el('div', 'k', 'Mapa niedostępna (brak internetu) — sama trasa.'));
  }

  async function drawMap(box, segs, st) {
    const m = el('div', 'map');
    box.appendChild(m);
    if (!(await loadLeaflet())) {
      m.remove();
      drawTrack(box, segs, st);
      return;
    }
    const L = root.L;
    const map = L.map(m, { attributionControl: true });
    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19, attribution: '© <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
    }).addTo(map);
    for (const pts of segs) L.polyline(pts.map(p => [p.lat, p.lon]), { color: '#ff7a1a', weight: 4 }).addTo(map);
    const first = segs[0][0], lastSeg = segs[segs.length - 1], last = lastSeg[lastSeg.length - 1];
    L.circleMarker([first.lat, first.lon], { radius: 7, color: '#101418', fillColor: '#4cd97b', fillOpacity: 1 }).addTo(map);
    L.circleMarker([last.lat, last.lon], { radius: 7, color: '#101418', fillColor: '#ff5a5a', fillOpacity: 1 }).addTo(map);
    const [a, b, c, d] = st.bbox;
    if (a === c && b === d) map.setView([a, b], 17);
    else map.fitBounds([[a, b], [c, d]], { padding: [24, 24], maxZoom: 18 });
  }

  function drawChart(box, st) {
    const ser = st.series;
    if (ser.length < 2) return;
    const W = 360, H = 150, l = 34, r = 8, t = 10, b = 22;
    const t0 = ser[0].t, t1 = ser[ser.length - 1].t, span = Math.max(t1 - t0, 1);
    const vmax = Math.max(5, ...ser.map(p => p.v));
    const step = vmax > 60 ? 20 : vmax > 25 ? 10 : 5, top = Math.ceil(vmax / step) * step;
    const x = tt => l + (tt - t0) / span * (W - l - r), y = v => t + (1 - v / top) * (H - t - b);
    const s = svg('svg', { viewBox: `0 0 ${W} ${H}`, class: 'chart' });
    for (let v = 0; v <= top; v += step) {
      s.appendChild(svg('line', { x1: l, x2: W - r, y1: y(v), y2: y(v), stroke: '#2a3138' }));
      const lab = svg('text', { x: l - 4, y: y(v) + 4, 'text-anchor': 'end', fill: '#7f8a94', 'font-size': 10 });
      lab.textContent = v;
      s.appendChild(lab);
    }
    const pts = ser.map(p => x(p.t).toFixed(1) + ',' + y(p.v).toFixed(1));
    s.appendChild(svg('polygon', { points: `${l},${y(0)} ${pts.join(' ')} ${x(t1)},${y(0)}`, fill: 'rgba(255,122,26,.18)' }));
    s.appendChild(svg('polyline', { points: pts.join(' '), fill: 'none', stroke: '#ff7a1a', 'stroke-width': 2 }));
    const hhmm = tt => new Date(tt).toLocaleTimeString('pl-PL', { hour: '2-digit', minute: '2-digit' });
    for (const [tt, anchor] of [[t0, 'start'], [t1, 'end']]) {
      const lab = svg('text', { x: x(tt), y: H - 6, 'text-anchor': anchor, fill: '#7f8a94', 'font-size': 10 });
      lab.textContent = hhmm(tt);
      s.appendChild(lab);
    }
    box.appendChild(el('div', 'k', 'Prędkość [km/h]' + (st.hasSpeed ? '' : ' — liczona z pozycji (starszy plik)')));
    box.appendChild(s);
  }

  async function showRide(name) {
    const view = $('ride');
    view.textContent = '';
    view.hidden = false;
    doc.body.classList.add('noscroll');
    const head = el('div', 'rhead');
    const back = el('button', '', '‹ Przejazdy');
    back.onclick = () => { if (history.length > 1) history.back(); else location.hash = ''; };
    head.append(back, el('span', 'rwhen', when(name)));
    view.appendChild(head);
    const f = files.find(x => x.name === name) || { name, size: 0 };
    let r;
    try {
      r = await ride(f);
    } catch (e) {
      view.appendChild(el('p', 'err', 'Nie udało się wczytać trasy.'));
      return;
    }
    const st = r.stats;
    if (!st.points) {
      view.appendChild(el('p', 'k', 'Trasa nie ma jeszcze punktów.'));
    } else {
      await drawMap(view, r.segs, st);
      const g = el('div', 'grid stats');
      const cell = (k, v) => { const c = el('div'); c.append(el('div', 'k', k), el('div', 'v', v)); g.appendChild(c); };
      cell('Dystans', km(st.distM));
      cell('Czas jazdy', dur(st.movingS));
      cell('Średnia', kmh(st.avgKmh));
      cell('Maks.', kmh(st.maxKmh));
      cell('Podjazdy ≈', Math.round(st.climbM) + ' m');
      cell('Całość', dur(st.totalS));
      view.appendChild(g);
      drawChart(view, st);
      const n = st.segments, n10 = n % 10, n100 = n % 100;
      const word = n === 1 ? 'odcinek' : n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14) ? 'odcinki' : 'odcinków';
      view.appendChild(el('div', 'k', st.points + ' punktów, ' + n + ' ' + word + (n > 1 ? ' (przerwy w sygnale)' : '')));
    }
    const act = el('div', 'ract');
    const dl = el('a', 'btn', 'Pobierz GPX');
    dl.href = '/gpx/' + encodeURIComponent(name);
    dl.setAttribute('download', name);
    act.appendChild(dl);
    if (f.active) {
      act.appendChild(el('small', '', 'zapisywany — nie można usunąć'));
    } else {
      const del = el('button', 'del', 'Usuń');
      del.onclick = async () => {
        if (!confirm('Usunąć przejazd z ' + when(name) + '?')) return;
        await fetch('/api/delete?name=' + encodeURIComponent(name), { method: 'POST' });
        try { root.localStorage.removeItem(CACHE + name + ':' + f.size); } catch (e) { /* */ }
        location.hash = '';
        refreshList();
      };
      act.appendChild(del);
    }
    view.appendChild(act);
  }

  function route() {
    const m = /^#r=(\d{8}-\d{6}\.gpx)$/.exec(location.hash);
    if (m) {
      showRide(m[1]);
    } else {
      $('ride').hidden = true;
      $('ride').textContent = '';
      doc.body.classList.remove('noscroll');
    }
  }

  function init() {
    root.addEventListener('hashchange', route);
    refreshList().then(route);
    setInterval(() => { if ($('ride').hidden) refreshList(); }, 15000);
  }

  root.CydRides = { init };
})(typeof window !== 'undefined' ? window : globalThis);
