// Hover on the luck chart, the unlock flow, and the refresh-rate buttons.
// node uitest.js page.html ../jsontest/out.json
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const js = html.match(/<script>([\s\S]*)<\/script>/)[1];
const full = JSON.parse(fs.readFileSync(process.argv[3], 'utf8').trim().split('\n')[1]);
Object.assign(full, { rate: 980, rssi: -44, worker: 'megaminer', state: 'mining', pool: '1.2.3.4:5', jobs: 12, vcc: 4.87, pw: 1 });
const els = {}; let texts = [];
const ctx = new Proxy({ fillText: (t) => texts.push(String(t)) }, { get: (o, k) => (k in o ? o[k] : () => {}), set: (o, k, v) => (o[k] = v, true) });
function el(id) { return els[id] || (els[id] = { id, clientWidth: 800, clientHeight: 260, scrollTop: 0, scrollHeight: 100, style: {}, title: '', disabled: false, value: '',
  handlers: {}, addEventListener(ev, f) { this.handlers[ev] = f; }, getBoundingClientRect: () => ({ left: 0 }), children: [], textContent: '', className: '', getContext: () => ctx,
  appendChild(c) { this.children.push(c); const m = /id=(\w+)/.exec(c.innerHTML || ''); if (m) el(m[1]); } }); }
const created = [];
global.document = { getElementById: el, createElement: () => { const b = { className: '' }; created.push(b); return b; }, createTextNode: () => ({}) };
global.window = {}; global.confirm = () => true;
const store = {};
global.localStorage = { getItem: k => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); }, removeItem: k => { delete store[k]; } };
const posts = []; let timers = []; let unmarked = 0;
global.fetch = (url, opt) => {
  if (url === '/status') return Promise.resolve({ ok: true, json: () => Promise.resolve(full) });
  if (url === '/cmd') {
    posts.push(opt.body);
    if (!opt.headers || opt.headers['X-Megaminer'] !== '1') unmarked++;
    const pass = decodeURIComponent(/pass=([^&]*)/.exec(opt.body)[1]);
    const ok = !full.pw || pass === 'sesame';
    return Promise.resolve({ ok, text: () => Promise.resolve(ok ? (opt.body.endsWith('cmd=') ? 'unlocked' : 'sent; the answer appears in the log') : 'refused: wrong console password') });
  }
  return Promise.resolve({ ok: true, headers: { get: h => h === 'X-Log-End' ? '5' : '1000' }, text: () => Promise.resolve('hello') });
};
global.setTimeout = (f, ms) => { timers.push(ms); return 1; }; global.clearTimeout = () => {};
eval(js);
const tick = async () => { for (let i = 0; i < 50; i++) await null; };
let fails = 0;
const check = (what, ok) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) fails++; };
(async () => {
  await tick();
  // --- lock ---
  check('locked: command box disabled', els.c.disabled && els.sd.disabled && els.r.disabled);
  check('locked: password field shown, unlocked bar hidden', els.lk.style.display === '' && els.ul.style.display === 'none');
  els.p.value = 'wrong'; els.u.onclick(); await tick();
  check('wrong password: stays locked, says so', els.c.disabled && els.m.textContent === 'wrong console password' && els.m.className === 'no');
  check('unlock sends an empty command', posts[0] === 'pass=wrong&cmd=');
  els.p.value = 'sesame'; els.f.onsubmit({ preventDefault() {} }); await tick();   // Enter in the password field
  check('right password (Enter): unlocked, field cleared', !els.c.disabled && !els.r.disabled && els.p.value === '' && els.m.textContent === 'unlocked');
  check('unlocked bar shown, password field hidden', els.lk.style.display === 'none' && els.ul.style.display === '');
  els.c.value = 'show'; els.f.onsubmit({ preventDefault() {} }); await tick();
  check('command carries the kept password', posts[posts.length - 1] === 'pass=sesame&cmd=show' && els.m.className === 'ok');
  full.pw = 1; // password changed on the miner: next command refused -> lock again
  const keep = global.fetch; global.fetch = (u, o) => u === '/cmd' ? (posts.push(o.body), Promise.resolve({ ok: false, text: () => Promise.resolve('refused: wrong console password') })) : keep(u, o);
  els.c.value = 'show'; els.f.onsubmit({ preventDefault() {} }); await tick();
  check('refused for the password: locked again', els.c.disabled && els.lk.style.display === '');
  global.fetch = keep;
  els.p.value = 'sesame'; els.u.onclick(); await tick();
  els.lo.onclick();
  check('every command carries the X-Megaminer header', unmarked === 0);
  check('lock button locks', els.c.disabled && els.m.textContent === 'locked');
  full.pw = 0; g(); await tick();
  check('no password set: open, note shown, no password field', !els.c.disabled && els.np.style.display === '' && els.lk.style.display === 'none' && els.ul.style.display === 'none');
  // --- hover on the luck chart ---
  texts = []; hd();
  check('no box without pointing', !texts.some(t => t.startsWith('seen ')));
  const bw = (800 - 34 - 6) / 28;
  texts = []; els.hg.handlers.pointermove({ clientX: 34 + 3.5 * bw }); // bar 3
  const box = texts.filter(t => /zero bits|^seen|^expected|^1 hash|^luck/.test(t));
  console.log('   box for bar 3:', JSON.stringify(box));
  const exp3 = full.hashes / 16;
  check('box names bar 3 and its count', box[0] === 'exactly 3 leading zero bits' && box[1] === 'seen ' + full.hist[3].toLocaleString());
  check('expected = hashes / 16', box[2] === 'expected ' + Math.round(exp3).toLocaleString());
  check('rarity line', /^1 hash in 2\^4, about every /.test(box[3]));
  full.hist = full.hist.concat(Array(41 - full.hist.length).fill(0)); full.hist[40] = 1; hd();
  check('40+ hash past 28 bars: caption says so', /1 hash past the last bar/.test(els.hc.textContent));
  els.bn.value = '99'; els.bn.onchange(); check('bars 99 -> 41, stored', String(els.bn.value) === '41' && store.bbn === '41');
  check('no hidden note at 41 bars', !/past the last bar/.test(els.hc.textContent));
  els.bn.value = '3'; els.bn.onchange(); check('bars 3 -> 8', String(els.bn.value) === '8');
  els.bn.value = 'x'; els.bn.onchange(); check('bars nonsense keeps 8', String(els.bn.value) === '8');
  els.bn.value = '41'; els.bn.onchange(); const bw41 = (800 - 34 - 6) / 41; texts = []; els.hg.handlers.pointermove({ clientX: 34 + 40.5 * bw41 });
  check('last bar is "40 or more"', texts.includes('40 or more leading zero bits'));
  texts = []; els.hg.handlers.pointerleave(); check('box gone after leaving', !texts.some(t => t.startsWith('seen ')));
  texts = []; els.hg.handlers.pointermove({ clientX: 10 }); check('left of the bars: no box', !texts.some(t => t.startsWith('seen ')));
  // --- refresh rate ---
  const pi = els.pi, pz = els.pz;
  check('default: 5 s, not paused', String(pi.value) === '5' && !pz.checked);
  const set = async (v) => { timers = []; pi.value = v; pi.onchange(); await tick(); };
  await set('12'); check('12 -> 12 s, stored, next poll in 12 s', String(pi.value) === '12' && store.bbp === '12' && timers.includes(12000));
  await set('0'); check('0 -> snaps to 1', String(pi.value) === '1' && timers.includes(1000));
  await set('99'); check('99 -> snaps to 30', String(pi.value) === '30' && timers.includes(30000));
  await set('2.6'); check('2.6 -> 3', String(pi.value) === '3');
  await set('abc'); check('nonsense -> keeps 3', String(pi.value) === '3');
  await set(''); check('empty -> keeps 3', String(pi.value) === '3');
  timers = []; pz.checked = true; pz.onchange(); await tick();
  check('paused: nothing scheduled, stored', els.s.textContent === 'paused' && store.bbz === '1' && timers.length === 0);
  timers = []; pz.checked = false; pz.onchange(); await tick();
  check('unpaused: polling again every 3 s', store.bbz === '0' && timers.includes(3000));
  console.log(fails ? fails + ' FAILED' : 'ALL OK');
})();
