/*
 * webpage.h - the page the miner serves at http://<its address>/ (see
 * net_winc.c). Kept in flash (PROGMEM): RAM is scarce, flash is not.
 *
 * It shows:
 *   - a status box: state, rate, averages, uptime, hashes, jobs, worker and
 *     pool, pool and network difficulty, best this run and ever (hover for
 *     the hash), WiFi signal, supply voltage; from /status (JSON, written by
 *     net_winc.c + the miner's status_json + hashrate_json), fetched after
 *     each /log;
 *   - the luck chart: a histogram of every hash by its leading zero bits
 *     (log scale), with the expected halving staircase and the luck lights'
 *     thresholds marked;
 *   - a hash rate chart: the browser picks the "rate | avg 10m, 1h, 1d" numbers
 *     out of the time-stamped rate lines in each /log fetch and keeps them in
 *     its own storage (localStorage, up to 24 h), so the history survives a
 *     reload in the same browser. Nothing about the chart is kept on the chip.
 *     Lines are broken where points are more than 30 s apart (page closed,
 *     miner restarting). Range buttons: 10 min, 1 h, 6 h, 24 h. The legend
 *     shows the latest values; pointing at the chart (mouse, or a finger on a
 *     phone) shows the values at that moment, Grafana style. "clear" asks
 *     first.
 *   - the miner's console output: on opening, the start-up part and the
 *     recent part (/log?from=0); then every 5 s only what is new
 *     (/log?from=<where the last answer ended>), appended, the box kept to
 *     the last ~200-300 KB and scrolled to the bottom unless you scroll up.
 *     After a miner restart the page marks it and shows the new start-up;
 *   - a command box and a reboot button; both POST to /cmd with the console
 *     password. With a password set they stay disabled until "unlock" (an
 *     empty command = a password check); the page then keeps the password in
 *     memory for this tab and sends it with each command. The answer stays
 *     shown (green: accepted, red: refused) until the next one.
 * Layout (2026-11-19): the rate chart and the luck chart side by side on a
 * wide screen, stacked on a narrow one; pointing at a luck bar shows its
 * numbers. "refresh every" N seconds (1-30) and a "paused" box, kept in
 * this browser.
 * A failed /log fetch is retried after 1 s and only reported after two
 * failures in a row (the miner serves two visitors at a time and can be
 * briefly busy). The empty icon link stops the browser asking for
 * /favicon.ico.
 */

#ifndef WEBPAGE_H
#define WEBPAGE_H

/* Not compiled into the chip itself any more (2026-11-19): the Makefile
 * builds tools/webpage_dump.c on the PC, which writes this page out, and
 * tools/gzip_header.py packs it into webpage_gz.h (12 KB -> ~5 KB of flash).
 * Edit the page here as before. */
#ifdef __AVR__
#include <avr/pgmspace.h>
#else
#define PROGMEM
#endif

static const char WEB_PAGE[] PROGMEM =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<link rel=icon href='data:,'>"
"<title>megaminer</title><style>"
"body{font:13px monospace;margin:12px;background:#111;color:#ddd}"
"pre{white-space:pre-wrap;word-break:break-all;background:#000;color:#cfc;padding:8px;"
"height:34vh;min-height:220px;overflow:auto;border:1px solid #333}"
"canvas{width:100%;height:34vh;min-height:220px;display:block;border:1px solid #333;margin:8px 0 4px;touch-action:pan-y}"
"input,button{font:inherit;background:#222;color:#ddd;border:1px solid #555;padding:3px}"
"button.on{border-color:#6d6;color:#6d6}:disabled{opacity:.4}"
"#s,#gl,#hc,#pn,#np,#dn{color:#999}#pn{font-size:11px}.ok{color:#6d6}.no{color:#f66}"
"#dn{float:right;margin-left:12px}#dn span{user-select:all;color:#ddd}"
"#sb{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:4px;margin:8px 0}"
"#sb div{background:#1a1a1a;border:1px solid #333;padding:4px 6px}#sb small{color:#888}"
"#ch{display:flex;flex-wrap:wrap;gap:0 12px}#ch>div{flex:1 1 440px;min-width:0}"
"</style></head><body>"
"<div id=dn>donations: <span>bc1qammq37l9n7dl7uetz4960c606mrzqe9l9h24zw</span></div>"
"<b>megaminer</b> <span id=ver></span> &middot; <span id=s>loading</span> &middot; refresh every "
"<input id=pi type=number min=1 max=30 step=1 style=width:4em> s "
"<label><input id=pz type=checkbox> paused</label>"
"<div id=pn>1-30 s. Each refresh takes the chip briefly away from hashing: shorter = fresher figures but a "
"little less hashing. (No longer than 30 s: the miner keeps only the last 4 KB of output.)</div>"
"<div id=sb></div>"
"<div id=ch><div><canvas id=g></canvas>"
"<div><span id=v1 style=color:#6d6></span> &nbsp;<span id=v2 style=color:#fc6></span> "
"&nbsp;<span id=v3 style=color:#69f></span> &nbsp;<span id=v4 style=color:#f66></span> "
"&middot; <span id=rg></span> <button type=button id=clr>clear</button> &middot; <span id=gl></span></div></div>"
"<div><canvas id=hg></canvas><div>bars <input id=bn type=number min=8 max=41 step=1 style=width:4em> "
"<span id=hc></span></div></div></div>"
"<pre id=l></pre>"
"<form id=f><span id=lk>password <input id=p type=password size=10> "
"<button type=button id=u>unlock</button> </span>"
"<span id=ul style=display:none>unlocked <button type=button id=lo>lock</button> </span>"
"<span id=np style=display:none>(no console password set) </span>"
"command <input id=c size=28 disabled placeholder='unlock first'> <button id=sd disabled>send</button> "
"<button type=button id=r disabled>reboot</button> <span id=m></span></form>"
"<div style=margin-top:6px>new firmware <input type=file id=uf accept=.bin> "
"<button type=button id=ub disabled>update</button> <span id=um></span></div>"
"<script>"
"var $=function(i){return document.getElementById(i)},l=$('l'),s=$('s'),m=$('m'),"
"p=$('p'),c=$('c'),bad=0,t,H=[],R=36e5,P=[],hv=-1,hb=-1,G={},K={},KEY=null,PS=5,PZ=0,"
"N=['rate','avg 10m','avg 1h','avg 1d'],C=['#6d6','#fc6','#69f','#f66'];"
"try{H=JSON.parse(localStorage.getItem('bbm')||'[]')}catch(e){H=[]}"
"function cl(v,d){var n=Math.round(+v);return v==null||v===''||isNaN(n)?d:Math.max(1,Math.min(n,30))}"
"try{PS=cl(localStorage.getItem('bbp'),5);PZ=localStorage.getItem('bbz')=='1'?1:0}catch(e){}"
"var PR=PZ?0:PS*1e3,BN=28;"
"try{var q=Math.round(+localStorage.getItem('bbn'));if(q>=8&&q<=41)BN=q}catch(e){}"
"function f1(v){return v==null?'-':(+v).toFixed(1)}"
/* rate lines look like: [18:51:07Z] rate 971.8 | avg 10m 571.6, 1h 89.9, 1d 16.9 | ... */
"function ingest(x){var re=/\\[(\\d\\d):(\\d\\d):(\\d\\d)Z\\] rate ([\\d.]+) \\| avg 10m ([\\d.]+), 1h ([\\d.]+), 1d ([\\d.]+)/g,"
"a,n=Date.now(),add=0;while((a=re.exec(x))){var d=new Date(n);d.setUTCHours(+a[1],+a[2],+a[3],0);"
"var u=d.getTime();if(u>n+6e4)u-=864e5;if(!H.length||u>H[H.length-1][0]){H.push([u,+a[4],+a[5],+a[6],+a[7]]);add=1}}"
"if(add){var o=n-864e5;while(H.length&&H[0][0]<o)H.shift();"
"try{localStorage.setItem('bbm',JSON.stringify(H))}catch(e){}}draw()}"
"function draw(){var g=$('g'),w=g.clientWidth,h=g.clientHeight||260,k=window.devicePixelRatio||1;"
"g.width=w*k;g.height=h*k;var x=g.getContext('2d');x.scale(k,k);x.fillStyle='#000';x.fillRect(0,0,w,h);"
"var n=Date.now(),t0=n-R,L=44,pw=w-L-8,T=8,ph=h-T-22,mx=100,i,j;"
"P=H.filter(function(q){return q[0]>=t0});G={t0:t0,L:L,pw:pw};if(hv>=P.length)hv=-1;"
"P.forEach(function(q){mx=Math.max(mx,q[1],q[2],q[3],q[4]||0)});mx=Math.ceil(mx*1.1/100)*100;"
"x.font='11px monospace';x.lineWidth=1;"
"for(i=0;i<=4;i++){var y=T+ph-ph*i/4;x.strokeStyle='#222';x.beginPath();x.moveTo(L,y);x.lineTo(L+pw,y);x.stroke();"
"x.fillStyle='#888';x.fillText(Math.round(mx*i/4),2,y+4)}"
"x.fillText(new Date(t0).toLocaleTimeString(),L,h-6);x.textAlign='right';x.fillText('now',L+pw,h-6);x.textAlign='left';"
"function X(q){return L+(q[0]-t0)/R*pw}function Y(v){return T+ph-v/mx*ph}"
"for(j=1;j<=4;j++){x.strokeStyle=C[j-1];x.lineWidth=j==1?1.5:1;x.beginPath();"
"var pv=0;P.forEach(function(q){if(q[j]==null){pv=0;return}"
"if(pv&&q[0]-pv<3e4)x.lineTo(X(q),Y(q[j]));else x.moveTo(X(q),Y(q[j]));pv=q[0]});x.stroke()}"
/* the pointed-at moment: a vertical line, a dot per value, and a box with the numbers */
"if(hv>=0){var q=P[hv],qx=X(q);x.strokeStyle='#777';x.lineWidth=1;x.beginPath();x.moveTo(qx,T);x.lineTo(qx,T+ph);x.stroke();"
"for(j=1;j<=4;j++)if(q[j]!=null){x.fillStyle=C[j-1];x.beginPath();x.arc(qx,Y(q[j]),3,0,7);x.fill()}"
"var bw=150,bx=qx+10+bw>L+pw?qx-10-bw:qx+10;x.fillStyle='rgba(20,20,20,.92)';x.fillRect(bx,T+4,bw,80);"
"x.strokeStyle='#444';x.strokeRect(bx,T+4,bw,80);x.fillStyle='#ddd';"
"x.fillText(new Date(q[0]).toLocaleTimeString(),bx+6,T+19);"
"for(j=1;j<=4;j++){x.fillStyle=C[j-1];x.fillText(N[j-1]+' '+f1(q[j]),bx+6,T+19+j*15)}}"
/* legend: the latest values */
"var z=H.length?H[H.length-1]:[];for(j=1;j<=4;j++)$('v'+j).textContent=N[j-1]+' '+f1(z[j]);"
"$('gl').textContent=P.length?P.length+' points shown':'no points yet: the chart fills while this page is open'}"
"function point(e){var r=$('g').getBoundingClientRect(),u=G.t0+(e.clientX-r.left-G.L)/G.pw*R,b=-1,bd=1e18,i;"
"for(i=0;i<P.length;i++){var d=Math.abs(P[i][0]-u);if(d<bd){bd=d;b=i}}hv=b;draw()}"
"$('g').addEventListener('pointermove',point);$('g').addEventListener('pointerdown',point);"
"$('g').addEventListener('pointerleave',function(){hv=-1;draw()});"
"var rg=$('rg');[['10m',6e5],['1h',36e5],['6h',216e5],['24h',864e5]].forEach(function(v){"
"var b=document.createElement('button');b.type='button';b.textContent=v[0];b.onclick=function(){R=v[1];hv=-1;"
"[].forEach.call(rg.children,function(e){e.className=''});b.className='on';draw()};"
"if(v[1]==R)b.className='on';rg.appendChild(b);rg.appendChild(document.createTextNode(' '))});"
/* how often to ask the miner: whole seconds 1-30 (anything else snaps back
 * into range), and a pause box; both kept in this browser */
"var pi=$('pi'),pz=$('pz');pi.value=PS;pz.checked=!!PZ;"
"function pset(){PS=cl(pi.value,PS);pi.value=PS;PZ=pz.checked?1:0;PR=PZ?0:PS*1e3;"
"try{localStorage.setItem('bbp',PS);localStorage.setItem('bbz',PZ)}catch(e){}"
"clearTimeout(t);if(PR)g();else s.textContent='paused'}"
"pi.onchange=pset;pz.onchange=pset;"
"$('clr').onclick=function(){if(confirm('Forget the chart history kept in this browser?')){H=[];hv=-1;"
"try{localStorage.removeItem('bbm')}catch(e){}draw()}};window.onresize=function(){draw();hd()};"
/* status box: /status (JSON) after each /log; one cell per figure */
"var S=null,F=[['state','x0'],['rate','x1'],['avg 10m / 1h / 1d','x2'],['up','x3'],['hashes','x4'],['jobs','x5'],"
"['worker @ pool','x6'],['pool difficulty','x7'],['network difficulty','x8'],['best this run','x9'],"
"['best ever','x10'],['WiFi signal','x11'],['supply','x12'],['last restart · watchdog','x13']];"
"F.forEach(function(f){var d=document.createElement('div');d.innerHTML='<small>'+f[0]+'</small><br><b id='+f[1]+'>-</b>';$('sb').appendChild(d)});"
"function sv(i,v,c){var e=$(i);e.textContent=v;if(c)e.style.color=c}"
"function dur(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+'d ':'')+h+'h'+(m<10?'0':'')+m+'m'}"
"function sci(v){return v>=1e6?v.toExponential(2):String(v)}"
"function st(){fetch('/status').then(function(r){if(!r.ok)throw 0;return r.json()}).then(function(j){S=j;"
"if(j.version)sv('ver',j.version+' ('+j.program+')');"
"sv('x0',j.state,{mining:'#6d6',connecting:'#fc6',retrying:'#fc6','needs you':'#f66'}[j.state]);sv('x1',f1(j.rate)+' H/s');"
"sv('x2',j.avg.map(f1).join(' / '));sv('x3',dur(j.up));sv('x4',j.hashes.toLocaleString());sv('x5',j.jobs);"
"sv('x6',j.worker+' @ '+j.pool);sv('x7',j.poolDiff!=null?j.poolDiff+' ('+j.poolBits+' zero bits)':'-');"
"sv('x8',j.netDiff?sci(j.netDiff)+' ('+j.netBits+' zero bits)':'-');"
"sv('x9',j.bestBits!=null?j.bestBits+' zero bits (expected '+j.expectedBits+', luck '+j.luck+'%)':'-');$('x9').title=j.best||'';"
"sv('x10',j.everBits!=null?j.everBits+' zero bits':'-');$('x10').title=j.ever||'';"
"sv('x11',j.rssi!=null?j.rssi+' dBm':'-');sv('x12',j.vcc.toFixed(2)+' V');"
"if(j.restart!=null){var wd=j.frozen+j.stuck;sv('x13',j.restart+' · '+(wd?j.frozen+' frozen, '+j.stuck+' stuck':'none needed'),wd?'#fc6':'#ddd');"
"$('x13').title='frozen: the watchdog restarted it after the program stopped for ~8 s; stuck: restarted after 15 minutes running without hashing'}"
"lockui();hd()}).catch(function(){})}"
/* luck chart: hashes by leading zero bits, log scale. A fair hash halves
 * from each bar to the next; white ticks = that expectation; green lines =
 * the luck lights' thresholds (8, 16, 24 bits). Pointing at a bar shows its
 * numbers. The last bin (40) counts 40 or more. */
"function ev(q){return q<1?Math.max(1,Math.round(q*1e3))+' ms':q<60?q.toFixed(q<10?1:0)+' s':q<3600?(q/60).toFixed(1)+' min':q<864e2?(q/3600).toFixed(1)+' h'"
":q<3156e4?(q/864e2).toFixed(1)+' days':(q/3156e4).toPrecision(3)+' years'}"
"function hd(){var g=$('hg'),w=g.clientWidth,h=g.clientHeight||260,k=window.devicePixelRatio||1;g.width=w*k;g.height=h*k;"
"var x=g.getContext('2d');x.scale(k,k);x.fillStyle='#000';x.fillRect(0,0,w,h);if(!S||!S.hist)return;"
"var A=S.hist,n=S.hashes||1,B=BN,L=34,pw=w-L-6,T=6,ph=h-T-18,bw=pw/B,top=Math.log2(n),i,hid=0;"
"for(i=B;i<A.length;i++)hid+=A[i]||0;"
"K={L:L,bw:bw,B:B};if(hb>=B)hb=-1;"
"function Y(v){return T+ph-(v>=1?ph*(Math.log2(v)+1)/(top+1):0)}"
"function E(i){return n/Math.pow(2,Math.min(i+1,40))}"
"x.font='10px monospace';x.fillStyle='#888';for(i=0;i<=top;i+=Math.max(1,Math.ceil(top/6)))x.fillText('2^'+i,0,Y(Math.pow(2,i))+3);"
"x.fillStyle='#264';[8,16,24].forEach(function(b){if(b<B)x.fillRect(L+b*bw,T,1,ph)});"
"for(i=0;i<B;i++){var v=A[i]||0,X=L+i*bw,e=E(i);x.fillStyle=i==hb?'#bfb':'#6d6';if(v)x.fillRect(X+1,Y(v),Math.max(1,bw-2),T+ph-Y(v));"
"if(i==hb&&!v){x.fillStyle='#333';x.fillRect(X+1,T,Math.max(1,bw-2),ph)}"
"if(e>=1){x.fillStyle='#fff';x.fillRect(X+1,Y(e),Math.max(1,bw-2),1)}if(i%4==0){x.fillStyle='#888';x.fillText(i,X,h-4)}}"
"if(hb>=0){var v=A[hb]||0,e=E(hb),z=Math.min(hb+1,40),r=S.rate,"
"y=[(hb>=40?'40 or more':'exactly '+hb)+' leading zero bits','seen '+v.toLocaleString(),"
"'expected '+(e>=10?Math.round(e).toLocaleString():e.toPrecision(2)),"
"'1 hash in 2^'+z+(r>0?', about every '+ev(Math.pow(2,z)/r):'')];"
"if(e>=1)y.push('luck '+Math.round(v/e*100)+'%');"
"var bx0=L+hb*bw+bw/2,bwd=270,bh=y.length*15+8,bx=bx0+10+bwd>w?bx0-10-bwd:bx0+10;if(bx<0)bx=0;"
"x.fillStyle='rgba(20,20,20,.92)';x.fillRect(bx,T+4,bwd,bh);x.strokeStyle='#444';x.strokeRect(bx,T+4,bwd,bh);"
"x.font='11px monospace';y.forEach(function(q,j){x.fillStyle=j?'#ddd':'#6d6';x.fillText(q,bx+6,T+18+j*15)})}"
"$('hc').textContent='hashes by leading zero bits (log scale): each bar about half the one before; white = expected; '"
"+'green lines = luck lights (8/16/24); point at a bar for its numbers'"
"+(S.poolBits?'. A share at this pool needs '+S.poolBits+' zero bits':'')"
"+(hid?'. '+hid.toLocaleString()+' hash'+(hid>1?'es':'')+' past the last bar (show more bars)':'')}"
/* how many bars (8-41; bar 40 counts 40 or more), kept in this browser */
"var bn=$('bn');bn.value=BN;bn.onchange=function(){var v=Math.round(+bn.value);"
"if(bn.value!==''&&!isNaN(v))BN=Math.max(8,Math.min(41,v));bn.value=BN;hb=-1;"
"try{localStorage.setItem('bbn',BN)}catch(e){}hd()};"
"function pb(e){var r=$('hg').getBoundingClientRect(),i=Math.floor((e.clientX-r.left-K.L)/K.bw);hb=i>=0&&i<K.B?i:-1;hd()}"
"$('hg').addEventListener('pointermove',pb);$('hg').addEventListener('pointerdown',pb);"
"$('hg').addEventListener('pointerleave',function(){hb=-1;hd()});"
/* log: ask only for what is new since the last answer (lp = position reached,
 * up = the miner's uptime then, lt = an unfinished last line kept back from
 * the chart parser). Uptime going backwards = the miner restarted: mark it and
 * start again from 0, which brings that run's start-up output. */
"var lp=0,up=-1,lt='';"
"function g(){clearTimeout(t);if(UP)return;var f=lp;fetch('/log?from='+f).then(function(r){if(!r.ok)throw 0;"
"var e=+r.headers.get('X-Log-End'),u=+r.headers.get('X-Up');return r.text().then(function(x){return[e,u,x]})})"
".then(function(a){bad=0;s.textContent='updated '+new Date().toLocaleTimeString();"
"if(f&&(a[1]<up||a[0]<f)){lp=0;up=-1;lt='';l.textContent+='\\n----- miner restarted -----\\n';t=setTimeout(g,0);return}"
"lp=a[0];up=a[1];var x=a[2];if(x){var e=l.scrollTop+l.clientHeight>=l.scrollHeight-30,v=l.textContent+x;"
"if(v.length>3e5)v=v.slice(v.indexOf('\\n',v.length-2e5)+1);l.textContent=v;if(e)l.scrollTop=l.scrollHeight;"
"x=lt+x;var k=x.lastIndexOf('\\n');lt=x.slice(k+1);x=x.slice(0,k+1)}ingest(x);st();"
"if(PR)t=setTimeout(g,PR)}).catch(function(){if(++bad>1)s.textContent='no answer from the miner (retrying)';"
"if(PR)t=setTimeout(g,bad>1?PR:1000)})}"
/* commands: the miner keeps no sessions (each request stands alone), so the
 * page checks the password once ("unlock": an empty command), keeps it in
 * this tab's memory and sends it with every command. Without a console
 * password the box is open. A refusal for the password locks again. */
"function lockui(){var need=!S||S.pw,open=!need||KEY!=null;"
"$('lk').style.display=need&&KEY==null?'':'none';$('ul').style.display=need&&KEY!=null?'':'none';"
"$('np').style.display=S&&!S.pw?'':'none';c.disabled=$('sd').disabled=$('r').disabled=$('ub').disabled=!open||UP>0;"
"c.placeholder=open?'show, help, ...':'unlock first'}"
"function say(k,v){m.className=k;m.textContent=v}"
"function post(k,w){return fetch('/cmd',{method:'POST',headers:{'X-Megaminer':'1'},body:'pass='+encodeURIComponent(w)+'&cmd='+encodeURIComponent(k)})"
".then(function(r){return r.text().then(function(y){return[r.ok,y]})})}"
"function unlock(){var w=p.value;say('','checking ...');post('',w).then(function(a){"
"if(a[0]){KEY=w;p.value='';say('ok','unlocked')}else say('no',a[1].replace(/^refused: /,''));lockui()})"
".catch(function(){say('no','no answer from the miner')})}"
"function x(k){say('','sending '+k+' ...');post(k,KEY||'').then(function(a){"
"if(!a[0]&&/wrong console password/.test(a[1])){KEY=null;lockui()}say(a[0]?'ok':'no',k+': '+a[1]);setTimeout(g,1500)})"
".catch(function(){say('no',k+': no answer'+(k=='reboot'?' (expected while it restarts)':''))})}"
"$('f').onsubmit=function(e){e.preventDefault();if($('lk').style.display!='none'){unlock();return}"
"if(c.value&&!c.disabled)x(c.value);c.value=''};"
"$('u').onclick=unlock;$('lo').onclick=function(){KEY=null;say('','locked');lockui()};"
"$('r').onclick=function(){if(confirm('Reboot the miner?'))x('reboot')};"
/* firmware update (boot.c): "update" restarts the miner into its bootloader,
 * which answers on this same address and port; wait for GET /boot to say
 * "bootloader", then POST /flash the .bin with its CRC-32 (and the console
 * password); the bootloader writes it, reads it back, checks the CRC and
 * starts it. The log polling pauses meanwhile and then picks up the restart. */
"var UP=0,CT=[];for(var i=0;i<256;i++){var q=i;for(var j=0;j<8;j++)q=q&1?0xEDB88320^(q>>>1):q>>>1;CT[i]=q>>>0}"
"function crc(u){var q=-1;for(var i=0;i<u.length;i++)q=CT[(q^u[i])&255]^(q>>>8);return(q^-1)>>>0}"
"function um(k,v){$('um').className=k;$('um').textContent=v}"
"function nap(ms){return new Promise(function(z){setTimeout(z,ms)})}"
"function wb(dl){return fetch('/boot',{cache:'no-store'}).then(function(r){return r.ok?r.text():''})"
".catch(function(){return''}).then(function(y){if(/^bootloader/.test(y))return;"
"if(Date.now()>dl)throw'the bootloader did not answer within 90 s';return nap(1000).then(function(){return wb(dl)})})}"
"function fl(u,k){return new Promise(function(ok,no){var q=new XMLHttpRequest();q.open('POST','/flash');"
"q.setRequestHeader('X-Crc32',k);if(KEY)q.setRequestHeader('X-Pass',KEY);"
"q.upload.onprogress=function(e){if(e.lengthComputable)um('','sending '+Math.round(e.loaded*100/e.total)+'% ...')};"
"q.onload=function(){(q.status==200?ok:no)(q.responseText.trim())};"
"q.onerror=function(){no('the connection broke while sending')};q.send(u)})}"
"$('ub').onclick=function(){var f=$('uf').files[0];if(!f){um('no','choose the .bin file first');return}"
"if(f.size<256||f.size>122880){um('no','not a miner firmware: '+f.size+' bytes (at most 122,880)');return}"
"if(!confirm('Send '+f.name+' ('+f.size.toLocaleString()+' bytes) to the miner? It restarts into its '"
"+'bootloader, writes and checks the new firmware, and restarts with it (about a minute).'))return;"
"f.arrayBuffer().then(function(b){var u=new Uint8Array(b),k=crc(u).toString(16);UP=1;clearTimeout(t);lockui();"
/* already in the bootloader (button menu, unfinished update)? then no "update" step */
"return fetch('/boot',{cache:'no-store'}).then(function(r){return r.ok?r.text():''}).catch(function(){return''})"
".then(function(y){if(/^bootloader/.test(y))return;"
"um('','asking the miner to restart into its bootloader ...');return post('update',KEY||'').then(function(a){"
"if(!a[0])throw a[1];um('','waiting for the bootloader (it joins the WiFi: 10-30 s) ...');return nap(3000)})"
".then(function(){return wb(Date.now()+9e4)})}).then(function(){um('','sending ...');return fl(u,k)})})"
".then(function(y){um('ok',y+'. The miner restarts; this page carries on in about 20 s.');UP=0;lockui();"
"t=setTimeout(g,2e4)}).catch(function(e){UP=0;lockui();um('no','update failed: '+e);t=setTimeout(g,5000)})};"
"draw();g();"
"</script></body></html>";

#endif
