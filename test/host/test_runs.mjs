// Host test for src/web/runs.js (history of acceleration runs). Run: sh test/host/run.sh
// The CSV lines below have the exact shape acc_hist_line() writes (checked in test_accel.c).
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { parseRunsCsv, summarize, parseTrace, plural, gradeText } = require(process.argv[2] || '../../src/web/runs.js');

let run = 0, fail = 0;
const check = (cond, what) => { run++; if (!cond) { fail++; console.log('FAIL', what); } };
const near = (a, b, tol) => Math.abs(a - b) <= tol;

const HEAD = 'id,utc,t0_50,t0_100,t0_200,t100_200,t100m,tquarter,v100m,vquarter,vmax,dist_m,dur_s,grade_pct,climb_m,aborted,low_rate,rate_hz\n';
const csv = HEAD +
  '20261124-153012,2026-11-24 15:30:12,2.35,6.50,,,7.91,,88.0,,120.3,180.4,9.87,,,0,0,10.0\n' +
  '20261124-154000,2026-11-24 15:40:00,2.20,6.00,,,7.70,14.10,90.1,141.0,150.0,402.4,15.20,-2.3,-9.2,0,0,10.0\n' +
  'R000003,,1.90,5.00,,,,,,,60.0,40.0,4.00,,,1,0,10.0\n' +     // interrupted: listed, not counted
  '20261124-160000,2026-11-24 16:00:00,2.60,7.00,,,,,,,110.0,150.0,9.00,0.2,0.3,0,1,1.0\n' + // 1 Hz
  '20261124-161000,2026-11-24 16:10:00,2.10,6.25,,,,,,,105.0,140.0,8.50,1.1,1.6,0,0,10.0\n';

// --- parse
{
  const r = parseRunsCsv(csv);
  check(r.length === 5, 'parse: 5 runs');
  check(r[0].id === '20261124-153012' && r[0].utc === '2026-11-24 15:30:12', 'parse: id and time');
  check(r[0].t[0] === 2.35 && r[0].t[1] === 6.5 && r[0].t[2] === null && r[0].t[4] === 7.91, 'parse: times, empty = null');
  check(r[0].vquarter === null && r[0].v100m === 88 && r[0].grade === null, 'parse: unknown values are null');
  check(r[1].grade === -2.3 && r[1].climb === -9.2, 'parse: grade');
  check(r[2].aborted && !r[2].lowRate && r[2].utc === '', 'parse: aborted run without GPS date');
  check(r[3].lowRate && r[3].rate === 1, 'parse: 1 Hz run');
  check(parseRunsCsv(HEAD).length === 0 && parseRunsCsv('').length === 0, 'parse: empty history');
  // columns found by name: a reordered / extended header still reads
  const other = 'rate_hz,id,new_col,t0_100\n10,R000001,x,5.5\n';
  const o = parseRunsCsv(other);
  check(o.length === 1 && o[0].id === 'R000001' && o[0].t[1] === 5.5 && o[0].t[0] === null, 'parse: columns by name');
  const neg = parseRunsCsv(HEAD + 'R000009,,-1.00,0.00,3.10,,,,,,,,,,,0,0,10\n')[0];
  check(neg.t[0] === null && neg.t[1] === null && neg.t[2] === 3.1, 'parse: -1 / 0 mean not reached');
  check(summarize([neg])[0].n === 0, 'summary: a -1 time is not a result');
  check(parseRunsCsv(HEAD.replace(/\n/g, '\r\n') + '20261124-153012,,1.00,,,,,,,,,,,,,0,0,10\r\n')[0].t[0] === 1, 'parse: CRLF');
}

// --- summary: only counted runs (not interrupted, not 1 Hz)
{
  const s = summarize(parseRunsCsv(csv));
  const s100 = s[1];
  check(s100.n === 3, 'summary: 3 counted 0-100 runs, got ' + s100.n);
  check(s100.best === 6.0 && s100.bestId === '20261124-154000', 'summary: best 0-100 (not the 5.00 of the interrupted run)');
  check(near(s100.mean, (6.5 + 6.0 + 6.25) / 3, 1e-9), 'summary: mean');
  check(near(s100.sd, 0.25, 1e-9), 'summary: sample standard deviation, got ' + s100.sd);
  check(s[5].n === 1 && s[5].best === 14.1 && s[5].sd === null, 'summary: one quarter-mile run, no spread');
  check(s[2].n === 0 && s[2].best === null && s[2].mean === null, 'summary: metric never reached');
}

// --- trace
{
  const t = parseTrace('t_s,kmh\n0.00,0.0\n0.10,1.8\n0.20,3.6\n\n');
  check(t.length === 3 && t[2].t === 0.2 && t[2].v === 3.6, 'trace: points');
  check(parseTrace('t_s,kmh\n0.1,x\n').length === 0, 'trace: bad line skipped');
}

// --- words
check(plural(1) === '1 pomiar' && plural(3) === '3 pomiary' && plural(5) === '5 pomiarów', 'plural 1/3/5');
check(plural(12) === '12 pomiarów' && plural(22) === '22 pomiary' && plural(0) === '0 pomiarów', 'plural 12/22/0');
check(gradeText(-2.34) === 'z górki 2,3 %' && gradeText(1.1) === 'pod górę 1,1 %', 'grade text');
check(gradeText(0.3) === 'płasko' && gradeText(null) === '', 'grade text: flat / unknown');

console.log(`${run - fail}/${run} checks passed`);
process.exit(fail ? 1 : 0);
