// Run the page's script against a fake DOM and canvas, feed it log text, check the parsed points.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
new Function(js); // syntax check
const els = {};
const ctx = new Proxy({}, { get: (o, k) => (k in o ? o[k] : () => {}), set: (o, k, v) => (o[k] = v, true) });
function el(id) { return els[id] || (els[id] = { id, clientWidth: 800, scrollTop: 0, clientHeight: 100, scrollHeight: 100,
  handlers: {}, addEventListener(ev, f) { this.handlers[ev] = f; }, getBoundingClientRect: () => ({ left: 0 }), children: [], textContent: '', className: '', getContext: () => ctx, appendChild(c) { this.children.push(c); } }); }
const store = { bbm: JSON.stringify([[Date.now()-40000, 970, 500, 70]]) };
global.document = { getElementById: el, createElement: () => ({}), createTextNode: () => ({}) };
global.window = {}; global.confirm = () => true;
global.localStorage = { getItem: k => store[k] || null, setItem: (k, v) => store[k] = v, removeItem: k => delete store[k] };
let pending = [];
global.fetch = () => new Promise(() => {}); // g() never resolves; we call ingest ourselves
global.setTimeout = () => 0; global.clearTimeout = () => {};
const now = new Date(); const hh = n => String(n).padStart(2, '0');
function stamp(sAgo) { const d = new Date(now - sAgo * 1000); return `[${hh(d.getUTCHours())}:${hh(d.getUTCMinutes())}:${hh(d.getUTCSeconds())}Z]`; }
const line = (sAgo, r, a10, a1h) => `${stamp(sAgo)} rate ${r} | avg 10m ${a10}, 1h ${a1h}, 1d 0.0 | best 00 (1 zero bits) | up 0h01m | hashes 5 | jobs 1\r\n`;
eval(js + `
;globalThis.T = { ingest, getH: () => H, draw };`);
const log1 = `[+0s] miner_sv2 starting\r\n` + line(20, 971.6, 523.3, 73.8) + `${stamp(16)} msg 0x1f 41 bytes\r\n` + line(15, 966.3, 539.4, 89.9) + line(10, 971.2, 539.4, 89.9);
T.ingest(log1);
console.log('after first fetch:', JSON.stringify(T.getH().map(p => p.slice(1))));
T.ingest(log1 + line(5, 971.8, 571.6, 89.9)); // overlap + one new line
console.log('after second fetch:', T.getH().length, 'points, last', T.getH().at(-1).slice(1).join(','));
console.log('stored in localStorage:', JSON.parse(store.bbm).length, 'points');
console.log('chart label:', els.gl.textContent);
console.log('times increasing:', T.getH().every((p, i, a) => i == 0 || p[0] > a[i - 1][0]), '; newest within 10 s of now:', Math.abs(now - T.getH().at(-1)[0]) < 10000);
// hover: point at the middle of the plotted area, then leave
const cv = els.g; const texts = [];
const realCtx = cv.getContext(); realCtx.fillText = (t) => texts.push(String(t));
cv.handlers.pointermove({ clientX: 44 + (800 - 44 - 8) * (1 - 12 / 3600) }); // ~12 s ago on a 1 h range
console.log('legend:', ['v1','v2','v3','v4'].map(i => els[i].textContent).join(' | '));
console.log('hover box text:', texts.filter(t => /rate|avg|:/.test(t)).slice(-5).join(' | '));
cv.handlers.pointerleave(); texts.length = 0; T.draw();
console.log('after leaving, box drawn:', texts.some(t => /^rate /.test(t)));
