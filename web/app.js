'use strict';

/* ---------- token & API ---------- */

const TOKEN_KEY = 'beam-token';
let token = null;
{
  const m = location.hash.match(/token=([0-9a-f]+)/);
  try {
    if (m) sessionStorage.setItem(TOKEN_KEY, m[1]);
    token = m ? m[1] : sessionStorage.getItem(TOKEN_KEY);
  } catch { token = m ? m[1] : null; }
  if (m) history.replaceState(null, '', location.pathname);   // keep the token out of the address bar
}

async function api(cmd, args = {}) {
  const res = await fetch('/api/' + cmd, {
    method: 'POST',
    headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json' },
    body: JSON.stringify(args),
  });
  const data = await res.json().catch(() => ({ error: 'bad response' }));
  if (res.status === 403) showLocked();
  if (data.error) throw new Error(data.error);
  return data;
}

/* ---------- helpers ---------- */

const $ = (id) => document.getElementById(id);

function h(tag, props = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') el.className = v;
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) el.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false) el.append(kid);
  return el;
}

function fmtBytes(n) {
  const u = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(n >= 100 ? 0 : 1) : Math.round(n)) + ' ' + u[i];
}

function fmtRate(bps) {
  const b = bps * 8;
  if (b >= 1e9) return (b / 1e9).toFixed(2) + ' Gbit/s';
  if (b >= 1e6) return (b / 1e6).toFixed(b >= 1e8 ? 0 : 1) + ' Mbit/s';
  if (b >= 1e3) return (b / 1e3).toFixed(0) + ' kbit/s';
  return '0 bit/s';
}

function fmtDur(s) {
  if (s == null || s < 0 || !isFinite(s)) return '—';
  s = Math.round(s);
  if (s >= 3600) return `${Math.floor(s / 3600)}h ${String(Math.floor(s / 60) % 60).padStart(2, '0')}m`;
  if (s >= 60) return `${Math.floor(s / 60)}m ${String(s % 60).padStart(2, '0')}s`;
  return s + 's';
}

function fmtWhen(ms) {
  const d = new Date(ms), now = new Date();
  const t = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return d.toDateString() === now.toDateString() ? t : d.toLocaleDateString([], { month: 'short', day: 'numeric' }) + ' ' + t;
}

function linkLabel(p) {
  if (p.speed >= 1000) return `${p.iface} · ${p.speed / 1000} Gbit/s`;
  if (p.speed > 0) return `${p.iface} · ${p.speed} Mbit/s`;
  return p.wireless ? `${p.iface} · Wi-Fi` : p.iface || '';
}

function toast(msg, kind = '') {
  const el = h('div', { class: 'toast ' + kind }, msg);
  $('toasts').append(el);
  setTimeout(() => el.remove(), kind === 'err' ? 7000 : 4000);
}

function showLocked() { $('locked').hidden = false; }

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

/* ---------- state ---------- */

let state = null;
let chosenTarget = null;      // peer key chosen for sending
const selection = new Set();  // absolute paths
let cwd = '';
let knownFinished = new Set();
const series = [];            // {out, in} bytes/s, one sample per update
const SERIES_LEN = 120;

/* ---------- peers ---------- */

function peerList() {
  if (!state) return [];
  const paired = state.peers.paired.map((p) => ({ ...p, paired: true, key: p.name }));
  const disc = state.peers.discovered;
  for (const p of paired) {
    const d = disc.find((x) => x.fp === p.fp);
    if (d) Object.assign(p, { iface: d.iface, speed: d.speed, wireless: d.wireless });
  }
  const unpaired = disc.filter((d) => !d.paired).map((d) => ({ ...d, paired: false, online: true, key: d.name, host: d.addr }));
  return [...paired.sort((a, b) => b.online - a.online), ...unpaired];
}

function renderPeers() {
  const list = peerList();
  const ul = $('peers');
  ul.replaceChildren();
  const online = list.filter((p) => p.online).length;
  $('peer-count').textContent = list.length ? `${online} online` : '';
  if (!list.length) {
    ul.append(h('li', { class: 'empty' }, 'No devices yet. Start beam on another machine on this network, then pair with it.'));
  }
  for (const p of list) {
    const actions = p.paired
      ? h('button', { class: 'btn ghost small danger', title: 'Forget this device', onclick: (e) => { e.stopPropagation(); unpair(p); } }, 'Unpair')
      : h('button', { class: 'btn small', onclick: (e) => { e.stopPropagation(); openJoin(p.name, p.name); } }, 'Pair');
    const meta = p.online
      ? `${p.host || p.addr}:${p.port}${linkLabel(p) ? ' · ' + linkLabel(p) : ''}`
      : `last seen at ${p.host || '?'}:${p.port} · offline`;
    const li = h('li', {
      class: 'peer' + (p.paired ? ' selectable' : '') + (chosenTarget === p.key ? ' chosen' : ''),
      title: p.fp,
      onclick: () => { if (p.paired) { chosenTarget = p.key; renderPeers(); renderTargets(); } },
    },
      h('span', { class: 'dot ' + (p.online ? 'on' : '') }),
      h('div', {},
        h('div', { class: 'name' }, p.name, h('span', { class: 'tag' + (p.paired ? '' : ' unpaired') }, p.paired ? 'paired' : 'new')),
        h('div', { class: 'meta' }, meta)),
      actions);
    ul.append(li);
  }
}

function renderTargets() {
  const sel = $('target');
  const list = peerList().filter((p) => p.paired);
  const unpaired = peerList().filter((p) => !p.paired);
  const prev = chosenTarget ?? sel.value;
  sel.replaceChildren();
  if (!list.length && !unpaired.length) sel.append(h('option', { value: '' }, '— no peers —'));
  for (const p of list) sel.append(h('option', { value: p.key }, `${p.name}${p.online ? '' : ' (offline)'}`));
  if (unpaired.length) {
    const g = h('optgroup', { label: 'Unpaired (needs a one-shot code)' });
    for (const p of unpaired) g.append(h('option', { value: p.key }, p.name));
    sel.append(g);
  }
  const manual = $('manual-host').value.trim();
  if (manual) sel.append(h('option', { value: manual }, manual));
  if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
  chosenTarget = sel.value || null;
  updateSendButton();
}

async function unpair(p) {
  if (!confirm(`Forget ${p.name}? You will need to pair again to send to it.`)) return;
  try { await api('unpair', { peer: p.fp }); toast(`Forgot ${p.name}`); } catch (e) { toast(e.message, 'err'); }
}

/* ---------- file browser ---------- */

const ICON_DIR = 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z';
const ICON_FILE = 'M6 3h8l5 5v13H6ZM14 3v5h5';

function icon(d) {
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('class', 'ic');
  const path = document.createElementNS(ns, 'path');
  path.setAttribute('d', d);
  svg.append(path);
  return svg;
}

async function browse(path) {
  try {
    const r = await api('ls', { path, hidden: $('show-hidden').checked });
    cwd = r.path;
    $('path').value = cwd;
    $('btn-up').dataset.parent = r.parent;
    const box = $('files');
    box.replaceChildren();
    if (!r.entries.length) box.append(h('div', { class: 'empty' }, 'Empty folder'));
    for (const e of r.entries) {
      const full = (cwd === '/' ? '' : cwd) + '/' + e.name;
      const cb = h('input', { type: 'checkbox', tabindex: '-1', 'aria-label': 'select ' + e.name });
      cb.checked = selection.has(full);
      const row = h('div', { class: 'frow' + (e.dir ? ' dir' : '') + (cb.checked ? ' sel' : ''), role: 'option', title: e.dir ? 'Double-click to open' : '' },
        cb, icon(e.dir ? ICON_DIR : ICON_FILE), h('span', { class: 'fn' }, e.name), h('span', { class: 'fs' }, e.dir ? '' : fmtBytes(e.size)));
      row.addEventListener('click', () => toggleSelect(full, row, cb));
      if (e.dir) row.addEventListener('dblclick', () => browse(full));
      box.append(row);
    }
    box.scrollTop = 0;
  } catch (err) {
    toast(err.message, 'err');
  }
}

function toggleSelect(full, row, cb) {
  if (selection.has(full)) selection.delete(full); else selection.add(full);
  if (row) { row.classList.toggle('sel', selection.has(full)); cb.checked = selection.has(full); }
  renderSelection();
}

function renderSelection() {
  const box = $('selected');
  box.replaceChildren();
  for (const p of selection) {
    const name = p.split('/').pop() || p;
    box.append(h('span', { class: 'chip', title: p }, h('span', {}, name),
      h('button', { 'aria-label': 'remove ' + name, onclick: () => { selection.delete(p); renderSelection(); browse(cwd); } }, '×')));
  }
  updateSendButton();
}

function updateSendButton() {
  $('btn-send').disabled = !selection.size || !$('target').value;
  $('btn-send').textContent = selection.size ? `Send ${selection.size} item${selection.size > 1 ? 's' : ''}` : 'Send';
  $('btn-bench').disabled = !$('target').value;
}

async function send(bench) {
  const to = $('target').value;
  const args = { to, streams: +$('streams').value || undefined };
  const code = $('send-code').value.trim();
  if (code) args.code = code;
  try {
    if (bench) {
      await api('bench', { ...args, secs: 10 });
      toast(`Benchmarking the link to ${to} for 10 s…`);
    } else {
      args.paths = [...selection];
      args.compress = $('compress').value;
      await api('send', args);
      toast(`Sending to ${to}…`);
      selection.clear();
      renderSelection();
      browse(cwd);
    }
    $('send-code').value = '';
  } catch (e) {
    toast(e.message, 'err');
  }
}

/* ---------- transfers ---------- */

const xferEls = new Map();

function sparkline(canvas, data, color) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, hgt = canvas.clientHeight;
  if (!w) return;
  canvas.width = w * dpr; canvas.height = hgt * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const max = Math.max(...data, 1);
  ctx.beginPath();
  data.forEach((v, i) => {
    const x = data.length > 1 ? (i / (data.length - 1)) * w : 0;
    const y = hgt - 2 - (v / max) * (hgt - 4);
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  });
  ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.stroke();
  ctx.lineTo(w, hgt); ctx.lineTo(0, hgt); ctx.closePath();
  ctx.globalAlpha = 0.12; ctx.fillStyle = color; ctx.fill();
}

function xferActions(t) {
  const box = h('div', { class: 'xfer-actions' });
  if (t.state === 'waiting' && t.direction === 'in') {
    box.append(h('button', { class: 'btn small primary', onclick: () => act('accept', t.id) }, 'Accept'),
               h('button', { class: 'btn small ghost', onclick: () => act('reject', t.id) }, 'Decline'));
  } else if (!['done', 'failed', 'cancelled', 'rejected'].includes(t.state)) {
    box.append(h('button', { class: 'btn small ghost danger', onclick: () => act('cancel', t.id) }, 'Cancel'));
  }
  return box;
}

async function act(cmd, id) {
  try { await api(cmd, { id }); } catch (e) { toast(e.message, 'err'); }
}

function renderTransfers() {
  const box = $('transfers');
  const list = state.transfers.filter((t) => !['done', 'failed', 'cancelled', 'rejected'].includes(t.state) ||
    Date.now() - t.finished < 120000);
  const ids = new Set(list.map((t) => t.id));
  for (const [id, el] of xferEls) if (!ids.has(id)) { el.root.remove(); xferEls.delete(id); }
  if (!list.length) {
    if (!box.querySelector('.empty')) box.replaceChildren(h('div', { class: 'empty' }, 'Nothing in flight. Pick files on the left and press Send.'));
    return;
  }
  box.querySelector('.empty')?.remove();

  // Oldest first, so prepending leaves the newest on top.
  for (const t of [...list].reverse()) {
    let el = xferEls.get(t.id);
    if (!el) {
      el = {};
      el.title = h('div', { class: 't' });
      el.sub = h('div', { class: 's' });
      el.state = h('span', { class: 'state' });
      el.actions = h('div');
      el.skip = h('div', { class: 'skip' });
      el.fill = h('div', { class: 'fill' });
      el.stats = h('div', { class: 'stats' });
      el.err = h('div', { class: 'err' });
      el.spark = h('canvas', { class: 'spark' });
      el.root = h('div', { class: 'xfer' },
        h('div', { class: 'xfer-top' },
          h('div', { class: 'xdir ' + t.direction }, t.direction === 'out' ? '↑' : '↓'),
          h('div', { class: 'xfer-title' }, el.title, el.sub), el.state, el.actions),
        h('div', { class: 'progress' }, el.skip, el.fill), el.stats, el.spark, el.err);
      box.prepend(el.root);
      xferEls.set(t.id, el);
    }
    const final = ['done', 'failed', 'cancelled', 'rejected'].includes(t.state);
    el.root.className = `xfer ${t.direction} ${t.state}`;
    el.title.textContent = t.title || '…';
    const where = t.direction === 'out' ? `to ${t.peer}` : `from ${t.peer}`;
    el.sub.textContent = t.bench ? `link benchmark ${where}` :
      `${where} · ${t.files} file${t.files === 1 ? '' : 's'}${t.direction === 'in' && t.dest ? ' → ' + t.dest : ''}`;
    el.state.textContent = t.state;
    el.state.className = 'state ' + t.state;
    const actKey = t.state + t.direction;
    if (el.actKey !== actKey) { el.actions.replaceChildren(xferActions(t)); el.actKey = actKey; }

    let frac = t.total > 0 ? t.done / t.total : (final && t.state === 'done' ? 1 : 0);
    if (t.bench) frac = t.state === 'done' ? 1 : Math.min(1, t.elapsed / Math.max(1, t.elapsed + Math.max(0, t.eta)));
    el.fill.style.width = (frac * 100).toFixed(2) + '%';
    el.skip.style.width = t.total > 0 && t.skipped ? ((t.skipped / t.total) * 100).toFixed(2) + '%' : '0';
    if (el.skip.style.width !== '0') el.fill.style.width = (Math.max(0, frac - t.skipped / t.total) * 100).toFixed(2) + '%';

    const parts = [];
    const stat = (label, value) => parts.push(h('span', {}, label + ' ', h('b', {}, value)));
    if (t.bench) {
      stat('moved', fmtBytes(t.done));
    } else {
      stat('', `${fmtBytes(t.done)} / ${fmtBytes(t.total)}`);
      stat('', (frac * 100).toFixed(1) + '%');
    }
    stat(final ? 'avg' : 'speed', fmtRate(t.rate));
    if (!final && t.eta >= 0) stat(t.bench ? 'left' : 'ETA', fmtDur(t.eta));
    if (final) stat('took', fmtDur(t.elapsed));
    if (t.ratio > 0 && t.ratio < 0.97) stat('zstd', Math.round((1 - t.ratio) * 100) + '% smaller');
    if (t.resumed) stat('resumed', fmtBytes(t.skipped));
    stat('streams', t.streams);
    el.stats.replaceChildren(...parts);
    el.err.textContent = t.error && t.state !== 'done' ? t.error : '';
    el.spark.hidden = !(t.hist && t.hist.length > 1);
    if (t.hist && t.hist.length > 1) sparkline(el.spark, t.hist, t.direction === 'out' ? css('--out') : css('--in'));
  }
}

/* ---------- chart ---------- */

function drawChart() {
  const canvas = $('chart');
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, hgt = canvas.clientHeight;
  canvas.width = w * dpr; canvas.height = hgt * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, hgt);

  const max = Math.max(1e6, ...series.map((s) => Math.max(s.out, s.in))) * 1.15;
  const padL = 64, padB = 18, padT = 6;
  const plotW = w - padL - 4, plotH = hgt - padB - padT;

  // grid + labels
  ctx.font = '11px system-ui, sans-serif';
  ctx.fillStyle = css('--muted');
  ctx.strokeStyle = css('--grid');
  ctx.lineWidth = 1;
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const y = padT + plotH - (i / 4) * plotH;
    ctx.beginPath(); ctx.moveTo(padL, y + 0.5); ctx.lineTo(w - 4, y + 0.5); ctx.stroke();
    ctx.fillText(fmtRate((max * i) / 4), padL - 8, y);
  }
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  for (const s of [60, 30, 0]) {
    const x = padL + plotW * (1 - s / 60);
    ctx.fillText(s ? `-${s}s` : 'now', Math.min(x, w - 16), padT + plotH + 4);
  }

  const line = (key, color) => {
    if (series.length < 2) return;
    const step = plotW / (SERIES_LEN - 1);
    const x0 = padL + plotW - (series.length - 1) * step;
    ctx.beginPath();
    series.forEach((s, i) => {
      const x = x0 + i * step, y = padT + plotH - (s[key] / max) * plotH;
      i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
    });
    ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.stroke();
    ctx.lineTo(x0 + (series.length - 1) * step, padT + plotH);
    ctx.lineTo(x0, padT + plotH);
    ctx.closePath();
    const g = ctx.createLinearGradient(0, padT, 0, padT + plotH);
    g.addColorStop(0, color + '55'); g.addColorStop(1, color + '00');
    ctx.fillStyle = g; ctx.fill();
  };
  line('in', css('--in'));
  line('out', css('--out'));
}

function sample() {
  let out = 0, inn = 0;
  for (const t of state.transfers) {
    if (t.state !== 'active' && t.state !== 'finishing') continue;
    if (t.direction === 'out') out += t.rate; else inn += t.rate;
  }
  series.push({ out, in: inn });
  if (series.length > SERIES_LEN) series.shift();
  $('rate-out').textContent = fmtRate(out);
  $('rate-in').textContent = fmtRate(inn);
  drawChart();
}

/* ---------- history ---------- */

async function loadHistory() {
  try {
    const r = await api('history');
    const tb = $('history');
    tb.replaceChildren();
    if (!r.history.length) tb.append(h('tr', {}, h('td', { colspan: '7', class: 'muted' }, 'No transfers yet')));
    for (const t of r.history.slice(0, 50)) {
      tb.append(h('tr', { title: t.error || t.dest || '' },
        h('td', { class: 'arrow ' + t.direction }, t.direction === 'out' ? '↑' : '↓'),
        h('td', { class: 'what' }, t.title),
        h('td', {}, t.peer),
        h('td', { class: 'num' }, t.bench ? '—' : fmtBytes(t.total)),
        h('td', { class: 'num' }, t.state === 'done' ? fmtRate(t.rate) : '—'),
        h('td', {}, fmtWhen(t.started)),
        h('td', { class: 'r-' + t.state }, t.state)));
    }
  } catch { /* shown via toast elsewhere */ }
}

/* ---------- dialogs: pairing & codes ---------- */

let codeTimer = null;

function closeDialog(d) { if (d.open) d.close(); }

async function openCode(kind) {
  const dlg = $('dlg-code');
  let r;
  try { r = await api(kind === 'pair' ? 'pair_start' : 'recv_code'); } catch (e) { toast(e.message, 'err'); return; }
  const pairedBefore = state ? state.peers.paired.length : 0;
  const lastId = state ? Math.max(0, ...state.transfers.map((t) => t.id)) : 0;
  $('code-title').textContent = kind === 'pair' ? 'Pair a device' : 'Receive with a one-shot code';
  $('code-help').textContent = kind === 'pair'
    ? 'Enter this code on the other machine. After pairing, the two devices recognise each other automatically.'
    : 'Give this code to the sender. It works once, for one transfer, without pairing.';
  $('code-value').textContent = r.code;
  $('code-cmd').textContent = kind === 'pair' ? `beam pair ${r.name} ${r.code}` : `beam send <files> --to ${r.name} --code ${r.code}`;
  $('code-status').textContent = 'Waiting…';
  const until = Date.now() + r.ttl * 1000;
  clearInterval(codeTimer);
  const tick = () => {
    const left = Math.max(0, until - Date.now());
    $('code-bar').style.width = (left / (r.ttl * 1000)) * 100 + '%';
    if (!dlg.open) { clearInterval(codeTimer); return; }
    if (kind === 'pair' && state.peers.paired.length > pairedBefore) {
      const p = state.peers.paired[state.peers.paired.length - 1];
      toast(`Paired with ${p.name}`, 'ok');
      closeDialog(dlg);
    } else if (kind === 'recv' && state.transfers.some((t) => t.id > lastId && t.direction === 'in')) {
      toast('Incoming transfer started', 'ok');
      closeDialog(dlg);
    } else if ((kind === 'pair' ? state.pair_ttl : state.recv_ttl) === 0 && Date.now() - (until - r.ttl * 1000) > 1500) {
      $('code-status').textContent = left ? 'The code was used by a failed attempt (wrong code?). Close and try again.' : 'Expired.';
      clearInterval(codeTimer);
    } else {
      $('code-status').textContent = `Waiting… ${fmtDur(left / 1000)} left`;
    }
  };
  codeTimer = setInterval(tick, 500);
  dlg.showModal();
  tick();
}

let joinTarget = null;
function openJoin(target, label) {
  joinTarget = target;
  $('join-name').textContent = label;
  $('join-code').value = '';
  $('join-error').textContent = '';
  $('dlg-join').showModal();
  $('join-code').focus();
}

async function doJoin() {
  const code = $('join-code').value.trim();
  if (!code) return;
  $('btn-join').disabled = true;
  $('join-error').textContent = '';
  try {
    const r = await api('pair_join', { to: joinTarget, code });
    toast(`Paired with ${r.name}`, 'ok');
    closeDialog($('dlg-join'));
  } catch (e) {
    $('join-error').textContent = e.message;
  } finally {
    $('btn-join').disabled = false;
  }
}

function openSettings() {
  const i = state.info;
  $('set-name').value = i.name;
  $('set-dir').value = i.download_dir;
  $('set-streams').value = i.streams;
  $('set-compress').value = i.compress === -1 ? 'auto' : i.compress === 0 ? 'off' : String(i.compress);
  $('set-auto').checked = i.auto_accept;
  $('set-error').textContent = '';
  $('ifaces').replaceChildren(h('div', {}, `Listening on TCP ${i.peer_port} · discovery UDP 7879 · ${i.cipher}`),
    ...i.interfaces.map((n) => h('div', {}, `${n.iface}  ${n.addr}  ${n.speed > 0 ? n.speed + ' Mbit/s' : n.wireless ? 'Wi-Fi' : ''}`)));
  $('dlg-settings').showModal();
}

async function saveSettings() {
  try {
    await api('config_set', {
      name: $('set-name').value.trim(),
      download_dir: $('set-dir').value.trim(),
      streams: +$('set-streams').value,
      compress: $('set-compress').value,
      auto_accept: $('set-auto').checked,
    });
    toast('Settings saved', 'ok');
    closeDialog($('dlg-settings'));
  } catch (e) {
    $('set-error').textContent = e.message;
  }
}

/* ---------- live updates ---------- */

let firstState = true;

function onState(s) {
  state = s;
  const i = s.info;
  $('dev-name').textContent = i.name;
  $('dev-fp').textContent = i.fp;
  $('dev-cipher').textContent = i.cipher;
  document.title = `beam · ${i.name}`;
  if (firstState) {
    firstState = false;
    $('streams').value = i.streams;
    $('compress').value = i.compress === -1 ? 'auto' : i.compress === 0 ? 'off' : String(i.compress);
    browse(i.home);
    loadHistory();
  }
  renderPeers();
  renderTargets();
  renderTransfers();
  sample();

  // Refresh history and notify when transfers finish.
  for (const t of s.transfers) {
    if (['done', 'failed', 'cancelled', 'rejected'].includes(t.state) && !knownFinished.has(t.id)) {
      if (knownFinished.size || Date.now() - t.finished < 3000) {
        if (t.state === 'done') toast(`${t.direction === 'out' ? 'Sent' : 'Received'} ${t.title} — ${fmtRate(t.rate)} avg`, 'ok');
        else toast(`${t.title}: ${t.state}${t.error ? ' — ' + t.error : ''}`, 'err');
      }
      knownFinished.add(t.id);
      loadHistory();
    }
  }
}

function connect() {
  const es = new EventSource('/api/events?token=' + encodeURIComponent(token));
  es.onopen = () => $('conn-dot').className = 'dot on';
  es.onmessage = (ev) => { try { onState(JSON.parse(ev.data)); } catch (e) { console.error(e); } };
  es.onerror = () => {
    $('conn-dot').className = 'dot off';
    // A 403 closes the stream for good; check whether the token is the problem.
    if (es.readyState === EventSource.CLOSED) {
      fetch('/api/events?token=' + encodeURIComponent(token)).then((r) => { if (r.status === 403) showLocked(); });
      setTimeout(connect, 2000);
    }
  };
}

/* ---------- wiring ---------- */

function init() {
  if (!token) { showLocked(); return; }
  $('btn-up').addEventListener('click', (e) => browse(e.currentTarget.dataset.parent || '/'));
  $('path').addEventListener('keydown', (e) => { if (e.key === 'Enter') browse($('path').value); });
  $('show-hidden').addEventListener('change', () => browse(cwd));
  $('target').addEventListener('change', () => { chosenTarget = $('target').value; renderPeers(); updateSendButton(); });
  $('manual-host').addEventListener('input', renderTargets);
  $('btn-manual-pair').addEventListener('click', () => {
    const host = $('manual-host').value.trim();
    if (host) openJoin(host, host); else $('manual-host').focus();
  });
  $('btn-send').addEventListener('click', () => send(false));
  $('btn-bench').addEventListener('click', () => send(true));
  $('btn-pair-code').addEventListener('click', () => openCode('pair'));
  $('btn-recv-code').addEventListener('click', () => openCode('recv'));
  $('btn-join').addEventListener('click', doJoin);
  $('join-code').addEventListener('keydown', (e) => { if (e.key === 'Enter') doJoin(); });
  $('btn-settings').addEventListener('click', openSettings);
  $('btn-save-settings').addEventListener('click', saveSettings);
  document.querySelectorAll('[data-close]').forEach((b) => b.addEventListener('click', () => closeDialog(b.closest('dialog'))));
  window.addEventListener('resize', () => state && drawChart());
  connect();
}

document.addEventListener('DOMContentLoaded', init);
