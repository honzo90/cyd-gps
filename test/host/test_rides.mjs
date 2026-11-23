// Host test for src/web/rides.js (the maths of the "Przejazdy" page). Run: sh test/host/run.sh
// Synthetic tracks only: real tracks contain the user's location and this repo is public.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { dist, parseGpx, rideStats } = require(process.argv[2] || '../../src/web/rides.js');

let run = 0, fail = 0;
const check = (cond, what) => { run++; if (!cond) { fail++; console.log('FAIL', what); } };
const near = (a, b, tol) => Math.abs(a - b) <= tol;

const M_LAT = 1 / 111195;  // 1 m north
const T0 = Date.UTC(2026, 10, 23, 13, 0, 0);
const iso = t => new Date(t).toISOString().replace('.000', '');

// Same text as gpx_point() in lib/gpx_core (with or without the speed extension).
function gpx(segs, withSpeed = true) {
  let s = '<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1" creator="CYD GPS" ' +
          'xmlns="http://www.topografix.com/GPX/1/1"\n     xmlns:gpxtpx="http://www.garmin.com/xmlschemas/TrackPointExtension/v2">\n' +
          ' <trk>\n  <name>x</name>\n  <trkseg>\n';
  segs.forEach((pts, i) => {
    if (i) s += '  </trkseg>\n  <trkseg>\n';
    for (const p of pts) {
      s += `   <trkpt lat="${(52 + p.n * M_LAT).toFixed(7)}" lon="21.0000000">` +
           (p.ele !== undefined ? `<ele>${p.ele.toFixed(1)}</ele>` : '') +
           `<time>${iso(T0 + p.s * 1000)}</time><sat>8</sat><hdop>1.1</hdop>` +
           (withSpeed ? `<extensions><gpxtpx:TrackPointExtension><gpxtpx:speed>${(p.v / 3.6).toFixed(2)}` +
                        '</gpxtpx:speed></gpxtpx:TrackPointExtension></extensions>' : '') + '</trkpt>\n';
    }
  });
  return s + '  </trkseg>\n </trk>\n</gpx>\n';
}

// walking north: 5 m every 4 s (4.5 km/h)
const walk = (n0, s0, count, ele = 100) =>
  Array.from({ length: count }, (_, i) => ({ n: n0 + 5 * i, s: s0 + 4 * i, v: 4.5, ele }));

// --- dist: same formula as odo_dist_m
check(near(dist(52, 21, 53, 21), 111195, 1), 'dist 1 deg lat');
check(dist(52, 21, 52, 21) === 0, 'dist zero');

// --- parse
{
  const segs = parseGpx(gpx([walk(0, 0, 3)]));
  check(segs.length === 1 && segs[0].length === 3, 'parse: 1 segment, 3 points');
  check(near(segs[0][0].v, 4.5, 0.02), 'parse: speed m/s -> km/h');
  check(segs[0][1].t - segs[0][0].t === 4000, 'parse: time');
  check(segs[0][0].ele === 100, 'parse: ele');
  const old = parseGpx(gpx([walk(0, 0, 2)], false));
  check(old[0][0].v === null, 'parse: old file has no speed');
  const writing = gpx([walk(0, 0, 4)]).replace('  </trkseg>\n </trk>\n</gpx>\n', '');  // cut mid-write
  check(parseGpx(writing)[0].length === 4, 'parse: file without closing tags');
  check(parseGpx('<gpx><trk><trkseg>\n  </trkseg></trk></gpx>').length === 0, 'parse: empty segment dropped');
}

// --- straight walk
{
  const st = rideStats(parseGpx(gpx([walk(0, 0, 10)])));
  check(st.points === 10 && st.segments === 1, 'walk: counts');
  check(near(st.distM, 45, 0.1), 'walk: 45 m, got ' + st.distM);
  check(st.movingS === 36 && st.totalS === 36, 'walk: 36 s moving');
  check(near(st.avgKmh, 4.5, 0.05), 'walk: avg 4.5, got ' + st.avgKmh);
  check(near(st.maxKmh, 4.5, 0.02) && st.hasSpeed, 'walk: max from Doppler speed');
  check(st.climbM === 0, 'walk: flat');
  check(st.bbox && near(st.bbox[2] - st.bbox[0], 45 * M_LAT, 2e-7), 'walk: bbox');
}

// --- a stop: 2 min without points, not moving time; total time includes it
{
  const st = rideStats(parseGpx(gpx([[...walk(0, 0, 5), ...walk(25, 136, 5)]])));
  check(near(st.distM, 45, 0.1), 'stop: distance continues, got ' + st.distM);
  check(st.movingS === 32, 'stop: moving 2 x 16 s, got ' + st.movingS);
  check(st.totalS === 152, 'stop: total includes the stop');
  check(near(st.avgKmh, st.distM / 32 * 3.6, 0.01), 'stop: average over moving time, not total');
  check(st.series.some(p => p.v === 0), 'stop: chart drops to 0');
}

// --- two segments: the jump between them is not distance
{
  const st = rideStats(parseGpx(gpx([walk(0, 0, 5), walk(1000, 30, 5)])));
  check(st.segments === 2, 'segments: 2');
  check(near(st.distM, 40, 0.1), 'segments: 2 x 20 m, got ' + st.distM);
}

// --- old file (no Doppler speed) with one position jump: max not the jump
{
  const pts = walk(0, 0, 12);
  pts.splice(6, 0, { n: 47, s: 21, v: 4.5, ele: 100 });  // 22 m in 1 s after (25 m, 20 s)
  const st = rideStats(parseGpx(gpx([pts], false)));
  check(!st.hasSpeed, 'old: no speed');
  check(st.maxKmh < 15, 'old: max over >= 5 s ignores a 1 s jump, got ' + st.maxKmh);
  check(st.maxKmh > 3, 'old: max still about walking pace, got ' + st.maxKmh);
  check(Math.max(...st.series.map(p => p.v)) < 15, 'old: the chart has no jump spike either');
}

// --- climb: noise does not count, a real hill does
{
  const noisy = walk(0, 0, 40).map((p, i) => ({ ...p, ele: 100 + (i % 2 ? 3 : -3) }));
  check(rideStats(parseGpx(gpx([noisy]))).climbM === 0, 'climb: +-3 m noise is 0');
  const hill = walk(0, 0, 41).map((p, i) => ({ ...p, ele: 100 + 2.5 * i }));  // +100 m
  const c = rideStats(parseGpx(gpx([hill]))).climbM;
  check(near(c, 100, 6), 'climb: 100 m hill, got ' + c);
  const down = walk(0, 0, 41).map((p, i) => ({ ...p, ele: 200 - 2.5 * i }));
  check(rideStats(parseGpx(gpx([down]))).climbM === 0, 'climb: downhill only is 0');
  // down 50 m, then up 20 m: only the 20 m climb counts (the reference follows the valley)
  const valley = walk(0, 0, 41).map((p, i) => ({ ...p, ele: i <= 20 ? 200 - 2.5 * i : 150 + (i - 20) }));
  const v = rideStats(parseGpx(gpx([valley]))).climbM;
  check(v >= 12 && v <= 20, 'climb: valley then 20 m up (hysteresis may drop < 5 m), got ' + v);
}

// --- empty
{
  const st = rideStats([]);
  check(st.points === 0 && st.distM === 0 && st.avgKmh === 0 && st.bbox === null, 'empty ride');
}

console.log(`${run - fail}/${run} checks passed`);
process.exit(fail ? 1 : 0);
