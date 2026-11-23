// The page's status box and luck chart against a /status reply made by the real hashrate.c (jsontest/out.json).
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
setTimeout = () => 0;
(async () => {
  for (let i = 0; i < 50; i++) await null;
  const cells = {}; for (let i = 0; i <= 12; i++) cells['x' + i] = els['x' + i].textContent;
  console.log(cells);
  console.log('state colour:', els.x0.style.color, '| best hash in tooltip:', els.x9.title);
  const bars = rects.filter(r => r[2] > 0 && r[3] > 2).length;
  console.log('histogram bars drawn:', bars, '| caption:', els.hc.textContent);
  console.log('y labels:', texts.filter(t => t.startsWith('2^')).join(' '));
})();
