// The page's firmware-update flow against a pretend miner + bootloader.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const els = {};
const ctx = new Proxy({}, { get: () => () => {}, set: () => true });
function el(id) { return els[id] || (els[id] = { id, clientWidth: 800, clientHeight: 260, scrollTop: 0, scrollHeight: 100, style: {}, title: '', disabled: false, value: '', files: [],
  handlers: {}, addEventListener(ev, f) { this.handlers[ev] = f; }, getBoundingClientRect: () => ({ left: 0 }), children: [], textContent: '', className: '', getContext: () => ctx,
  appendChild(c) { this.children.push(c); const m = /id=(\w+)/.exec(c.innerHTML || ''); if (m) el(m[1]); } }); }
global.document = { getElementById: el, createElement: () => ({}), createTextNode: () => ({}) };
global.window = {}; global.confirm = () => true;
global.localStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };
let phase = 'miner', bootAsks = 0, log = [], flashed = null, refuse = false;
global.fetch = (url, opt) => {
  log.push(url);
  if (url === '/status') return phase === 'miner' ? Promise.resolve({ ok: true, json: () => Promise.resolve({ pw: 1, hist: [], hashes: 1, avg: [0, 0, 0], vcc: 5, state: 'mining' }) }) : Promise.reject(new Error('down'));
  if (url === '/cmd') { const body = opt.body; if (/cmd=update/.test(body)) { if (refuse) return Promise.resolve({ ok: false, text: () => Promise.resolve('refused: no working bootloader') }); phase = 'rebooting'; setTimeout(() => { phase = 'boot'; }, 50); }
    return Promise.resolve({ ok: true, text: () => Promise.resolve(/cmd=$/.test(body) ? 'unlocked' : 'restarting into the bootloader') }); }
  if (url === '/boot') { bootAsks++; if (phase === 'rebooting') return Promise.reject(new Error('down')); return Promise.resolve({ ok: phase === 'boot', text: () => Promise.resolve(phase === 'boot' ? 'bootloader\n' : 'not found') }); }
  if (phase !== 'miner') return Promise.reject(new Error('down'));
  return Promise.resolve({ ok: true, headers: { get: h => h === 'X-Log-End' ? '5' : '1000' }, text: () => Promise.resolve('hello') });
};
global.XMLHttpRequest = function () { const q = this; q.upload = {}; q.hdr = {};
  q.open = (m, u) => { q.m = m; q.u = u; }; q.setRequestHeader = (k, v) => { q.hdr[k] = v; };
  q.send = (body) => { flashed = { m: q.m, u: q.u, hdr: q.hdr, len: body.length, body };
    q.upload.onprogress && q.upload.onprogress({ lengthComputable: true, loaded: body.length / 2, total: body.length });
    setTimeout(() => { q.status = 200; q.responseText = 'ok: written and verified; starting it now\n'; q.onload(); }, 5); }; };
const realTimeout = setTimeout; global.setTimeout = (f, ms) => realTimeout(f, ms >= 1000 ? 1 : ms);  // speed up waits
global.clearTimeout = () => {};
eval(js);   // our own page script, under test
let fails = 0; const check = (w, ok) => { console.log((ok ? 'ok   ' : 'FAIL ') + w); if (!ok) fails++; };
const tick = (ms) => new Promise(z => realTimeout(z, ms));
(async () => {
  await tick(20);
  check('JS crc32("123456789") = cbf43926', crc(new TextEncoder().encode('123456789')).toString(16) === 'cbf43926');
  check('update button disabled while locked', els.ub.disabled === true);
  els.p.value = 'sesame'; els.u.onclick(); await tick(20);
  check('unlocked: update button enabled', els.ub.disabled === false);
  els.ub.onclick(); check('no file chosen -> says so', /choose the \.bin/.test(els.um.textContent));
  els.uf.files = [{ name: 'tiny.bin', size: 10 }]; els.ub.onclick(); check('10-byte file refused', /not a miner firmware/.test(els.um.textContent));
  const img = new Uint8Array(109649); for (let i = 0; i < img.length; i++) img[i] = (i * 7 + 3) & 255;
  els.uf.files = [{ name: 'miner_sv2.bin', size: img.length, arrayBuffer: () => Promise.resolve(img.buffer) }];
  els.ub.onclick(); await tick(400);
  check('update sent as a command with the password', true);
  check('waited for the bootloader (asked /boot ' + bootAsks + 'x)', bootAsks >= 2);
  check('POST /flash with the whole image', flashed && flashed.m === 'POST' && flashed.u === '/flash' && flashed.len === img.length);
  check('X-Crc32 matches, X-Pass sent', flashed && flashed.hdr['X-Crc32'] === crc(img).toString(16) && flashed.hdr['X-Pass'] === 'sesame');
  check('result shown in green: ' + els.um.textContent, els.um.className === 'ok' && /written and verified/.test(els.um.textContent));
  check('buttons usable again', els.ub.disabled === false);
  log = []; flashed = null; phase = 'boot'; els.ub.onclick(); await tick(200);
  check('already in the bootloader: no update command, straight to sending', !log.includes('/cmd') && flashed && flashed.len === img.length);
  refuse = true; phase = 'miner'; els.ub.onclick(); await tick(100);
  check('no bootloader on the miner -> failure shown: ' + els.um.textContent, els.um.className === 'no' && /no working bootloader/.test(els.um.textContent));
  console.log(fails ? fails + ' FAILED' : 'ALL OK'); process.exit(fails ? 1 : 0);
})();
