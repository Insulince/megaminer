// The new "last restart / watchdog" cell (2026-11-20).
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const full = JSON.parse(fs.readFileSync(process.argv[3], 'utf8').trim().split('\n')[1]);
Object.assign(full, { rssi: -44, worker: 'megaminer', state: 'mining', pool: '192.168.1.165:23407', jobs: 12, vcc: 4.87 });
const els = {}; const rects = [], texts = [];
const ctx = new Proxy({ fillRect: (...a) => rects.push(a), fillText: (t) => texts.push(String(t)) }, { get: (o, k) => (k in o ? o[k] : () => {}), set: (o, k, v) => (o[k] = v, true) });
function el(id) { return els[id] || (els[id] = { id, clientWidth: 800, scrollTop: 0, clientHeight: 100, scrollHeight: 100, style: {}, title: '',
  handlers: {}, addEventListener(ev, f) { this.handlers[ev] = f; }, getBoundingClientRect: () => ({ left: 0 }), children: [], textContent: '', className: '', getContext: () => ctx,
  appendChild(c) { this.children.push(c); const m = /id=(\w+)/.exec(c.innerHTML || ''); if (m) el(m[1]); } }); }
global.document = { getElementById: el, createElement: () => ({}), createTextNode: () => ({}) };
global.window = {}; global.confirm = () => true;
global.localStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };
global.fetch = (url) => url === '/status'
  ? Promise.resolve({ ok: true, json: () => Promise.resolve(full) })
  : Promise.resolve({ ok: true, headers: { get: h => h === 'X-Log-End' ? '5' : '1000' }, text: () => Promise.resolve('hello') });
global.setTimeout = () => 0; global.clearTimeout = () => {};
eval(js);
const cases = [
  ['nothing to report', { restart: 'power on', frozen: 0, stuck: 0 }, c => c.t === 'power on · none needed' && c.col === '#ddd'],
  ['restarts counted', { restart: 'FROZEN', frozen: 2, stuck: 1 }, c => c.t === 'FROZEN · 2 frozen, 1 stuck' && c.col === '#fc6' && /~8 s/.test(c.title)],
  ['older miner (no fields)', { restart: undefined, frozen: undefined, stuck: undefined }, c => c.t === '-'],
];
(async () => {
  let bad = 0;
  for (const [name, extra, ok] of cases) {
    delete els.x13; el('x13').textContent = '-';
    Object.assign(full, extra);
    for (const k of ['restart', 'frozen', 'stuck']) if (extra[k] === undefined) delete full[k];
    st();
    for (let i = 0; i < 20; i++) await null;
    const c = { t: els.x13.textContent, col: els.x13.style.color, title: els.x13.title };
    const pass = ok(c);
    if (!pass) bad++;
    console.log((pass ? 'ok   ' : 'FAIL ') + name + ': ' + JSON.stringify(c.t) + ' ' + (c.col || ''));
  }
  console.log('cell exists in the box:', !!els.sb.children.find(d => /id=x13/.test(d.innerHTML)));
  console.log(bad ? 'SOME FAILED' : 'ALL OK');
})();
