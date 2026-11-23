// The page's incremental /log polling against a fake miner that answers like
// net_winc.c: /log?from=N -> bytes from N on, X-Log-End, X-Up.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
new Function(js);
const els = {};
const ctx = new Proxy({}, { get: (o, k) => (k in o ? o[k] : () => {}), set: (o, k, v) => (o[k] = v, true) });
function el(id) { return els[id] || (els[id] = { id, clientWidth: 800, scrollTop: 0, clientHeight: 100, scrollHeight: 100,
  handlers: {}, addEventListener(ev, f) { this.handlers[ev] = f; }, getBoundingClientRect: () => ({ left: 0 }), children: [], textContent: '', className: '', getContext: () => ctx, appendChild(c) { this.children.push(c); } }); }
const store = {};
global.document = { getElementById: el, createElement: () => ({}), createTextNode: () => ({}) };
global.window = {}; global.confirm = () => true;
global.localStorage = { getItem: k => store[k] || null, setItem: (k, v) => store[k] = v, removeItem: k => delete store[k] };

// fake miner
let log = '', upMs = 1000, offline = false; const asked = [];
global.fetch = (url) => {
  if (url === '/status') return Promise.reject(new Error('not in this test'));
  asked.push(url);
  if (offline) return Promise.reject(new Error('down'));
  let from = +(url.split('from=')[1] || 0); if (from > log.length) from = 0;
  const body = log.slice(from), end = log.length, up = upMs;
  return Promise.resolve({ ok: true, headers: { get: h => h === 'X-Log-End' ? String(end) : String(up) }, text: () => Promise.resolve(body) });
};
let timer = null; global.setTimeout = (f) => (timer = f, 1); global.clearTimeout = () => {};
const tick = async () => { const f = timer; timer = null; f && f(); for (let i = 0; i < 20; i++) await null; };

const hh = n => String(n).padStart(2, '0');
const t0 = Date.now() - 100000; const stamp = () => { const d = new Date(t0 + n * 5000); return `[${hh(d.getUTCHours())}:${hh(d.getUTCMinutes())}:${hh(d.getUTCSeconds())}Z]`; };
let n = 0;
const rate = () => `${stamp()} rate 97${n++}.0 | avg 10m 1.0, 1h 2.0, 1d 3.0 | best x\r\n`;

let ok = true; const check = (what, c) => { console.log((c ? 'ok   ' : 'FAIL ') + what); ok = ok && c; };
(async () => {
  log = 'START-UP\r\n' + rate();
  eval(js.replace('draw();g();', '') + ';globalThis.T={g,getH:()=>H};');
  T.g(); for (let i = 0; i < 20; i++) await null;
  check('first fetch asks from=0', asked.at(-1) === '/log?from=0');
  check('box shows start-up + rate', els.l.textContent === log);
  const end1 = log.length; log += rate(); upMs += 5000;
  await tick();
  check('second fetch asks from the previous end', asked.at(-1) === '/log?from=' + end1);
  check('box = whole log, nothing repeated', els.l.textContent === log && (els.l.textContent.match(/START-UP/g) || []).length === 1);
  // nothing new
  upMs += 5000; await tick();
  check('no new output: box unchanged', els.l.textContent === log);
  // a line split across two answers
  const partial = rate(); log += partial.slice(0, 20); upMs += 5000; await tick();
  log += partial.slice(20); upMs += 5000; await tick();
  check('split line shown whole', els.l.textContent === log);
  console.log('H:', JSON.stringify(T.getH().map(p=>[p[0]-t0,p[1]])));
  check('chart got 3 points incl. the split line (972), each once', T.getH().length === 3 && T.getH()[2][1] === 972);
  // miner reboots: uptime goes back, log restarts (shorter)
  offline = true; await tick(); await tick();
  check('offline reported after 2 failures', /no answer/.test(els.s.textContent));
  offline = false; log = 'START-UP 2\r\n'; upMs = 2000; await tick();
  check('restart detected: marker added', /miner restarted/.test(els.l.textContent));
  await tick();
  check('refetched from 0 after restart', asked.at(-1) === '/log?from=0');
  check('new start-up shown after the marker', els.l.textContent.endsWith('----- miner restarted -----\nSTART-UP 2\r\n'));
  // restart where the new log is already LONGER than our position: caught by uptime
  log += rate(); upMs += 5000; await tick();
  const before = els.l.textContent.length;
  log = 'START-UP 3\r\n' + 'x'.repeat(5000) + '\r\n'; upMs = 3000; await tick(); await tick();
  check('restart caught by uptime alone', els.l.textContent.endsWith('----- miner restarted -----\n' + log) && els.l.textContent.length > before);
  console.log(ok ? 'ALL OK' : 'SOME FAILED'); process.exit(ok ? 0 : 1);
})();
