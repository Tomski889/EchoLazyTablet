// EchoLazyTablet installer window. Talks to webapp.py; every call carries the token from the URL.
const TOKEN = location.hash.slice(1);
const $ = (id) => document.getElementById(id);
let view = null;
let showResult = false;
let previewKey = '';

const ICONS = {
  ok: '<svg viewBox="0 0 24 24"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>',
  warn: '<svg viewBox="0 0 24 24"><path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17.5v.01"/></svg>',
  bad: '<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg>',
  folder: '<path d="M3 7a2 2 0 012-2h4l2 2h8a2 2 0 012 2v8a2 2 0 01-2 2H5a2 2 0 01-2-2z" />',
  install: '<path d="M12 3v12m0 0l-5-5m5 5l5-5M4 19h16" />',
  repair: '<path d="M14.7 6.3a4 4 0 00-5.4 5.4L3 18l3 3 6.3-6.3a4 4 0 005.4-5.4l-2.6 2.6-2.4-.6-.6-2.4z" />',
};
// The installer's wrong-build message (patcher.check); other messages may say "rebuild".
const WRONG_BUILD = /Echo VR build is not the one/;
const STAGES = {
  install: [['check', 'Checking your game'], ['build', 'Building your PLAY tab'], ['write', 'Installing the tab'],
            ['verify', 'Double-checking'], ['plugin', 'Adding the EchoLazyTablet plugin'],
            ['graphics', 'Making the LOW graphics files (a minute or two)']],
  uninstall: [['check', 'Checking your game'], ['write', 'Taking the PLAY tab out'], ['plugin', 'Removing the plugin']],
};

async function api(path, body) {
  const r = await fetch('/api/' + path, {
    method: body === undefined ? 'GET' : 'POST',
    headers: { 'X-Token': TOKEN, 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return r.json();
}

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function situation(v) {
  const s = v.scan;
  if (!s && v.scanning) return { kind: 'busy', title: 'Getting ready…', text: 'Looking at your game and its hand tablet.' };
  if (!s || s.found === false) return { kind: 'bad', title: "Can't find Echo VR", text: 'Show us where it is: pick the folder called ready-at-dawn-echo-arena.', action: 'browse' };
  const p = s.problems.join(' ');
  if (WRONG_BUILD.test(p)) return { kind: 'bad', title: 'Different Echo VR version', text: s.problems.find((x) => WRONG_BUILD.test(x)) };
  if (/EchoLoader/.test(p)) return { kind: 'bad', title: 'EchoLoader needed', text: 'Install EchoLoader first: EchoLazyTablet runs as one of its plugins.' };
  if (s.problems.some((x) => !/running/.test(x))) return { kind: 'bad', title: "Can't add the tab", text: s.problems.filter((x) => !/running/.test(x)).join('\n') };
  const running = /running/.test(p);
  if (s.state.includes('now EchoLazyTablet')) return { kind: 'warn', title: 'EchoQueue is now EchoLazyTablet', text: running ? 'Close Echo VR, then update.' : 'Update to the new version. Your settings are kept.', action: 'repair' };
  if (s.state.includes('Repair')) return { kind: 'warn', title: 'Needs a quick repair', text: running ? 'Close Echo VR, then repair.' : 'Another mod changed your tablet. Repair puts PLAY back.', action: 'repair' };
  if (s.state.startsWith('installed')) return { kind: 'ok', title: "You're all set", text: running ? 'EchoLazyTablet is installed. Close Echo VR to make changes.' : `EchoLazyTablet ${v.version} is installed. Repair if your tab ever goes missing.`, action: 'repair', installed: true };
  return { kind: running ? 'warn' : 'ready', title: 'Ready to install', text: running ? 'Close Echo VR first, then install.' : 'One click adds the PLAY tab to your hand tablet.', action: 'install' };
}

function renderMain(v) {
  const s = v.scan;
  const sit = situation(v);
  const icon = $('stateicon');
  icon.className = 'state-icon ' + (sit.kind === 'ready' ? '' : sit.kind);
  icon.innerHTML = sit.kind === 'busy' ? '' : ICONS[sit.kind === 'ready' ? 'ok' : sit.kind] || '';
  if (sit.kind === 'ready') icon.innerHTML = '<svg viewBox="0 0 24 24"><path d="M12 3v12m0 0l-5-5m5 5l5-5M4 19h16"/></svg>';
  $('title').textContent = sit.title;
  $('subtitle').textContent = sit.text;

  const pills = $('pills');
  pills.replaceChildren();
  if (s && s.found !== false) {
    const p = s.problems.join(' ');
    const pill = (ok, text) => { const e = el('span', 'pill' + (ok ? '' : ' bad')); const i = el('i', '', ok ? '✓' : '!'); e.append(i, text); pills.append(e); };
    pill(true, 'Echo VR found');
    pill(!WRONG_BUILD.test(p), 'Right version');
    pill(!/EchoLoader/.test(p), 'EchoLoader');
    pill(!/running/.test(p), /running/.test(p) ? 'Close Echo VR' : 'Echo closed');
  }

  const busy = v.job.busy || v.scanning;
  const blocked = !sit.action || /running/.test(s && s.problems ? s.problems.join(' ') : '');
  $('install').disabled = busy || blocked;
  $('ctatext').textContent = sit.action === 'browse' ? 'Find my Echo VR folder' : sit.action === 'repair' ? (sit.installed ? 'Repair EchoLazyTablet' : 'Repair now') : 'Install EchoLazyTablet';
  $('ctaicon').innerHTML = sit.action === 'browse' ? ICONS.folder : sit.action === 'repair' ? ICONS.repair : ICONS.install;
  $('uninstall').hidden = !(s && s.state && (s.state.startsWith('installed') || s.state.includes('Repair')));
  $('uninstall').disabled = busy;
  $('rescan').disabled = busy;

  $('path').textContent = v.game || 'Not found';
  $('path').title = v.game || '';
  const mods = s && s.found !== false ? [...(s.echomods ? ['Echo Mods'] : []), ...(s.plugins || []).map((m) => m.replace(/\.dll$/i, ''))] : [];
  $('modsfield').hidden = !mods.length;
  $('mods').replaceChildren(...mods.map((m) => el('span', '', m)));
  const notes = s && s.found !== false ? [...(s.echomods ? ['Echo Mods found: EchoLazyTablet is switched on in its profiles, so Echo Mods keeps it on.'] : []), ...(s.notes || [])] : [];
  $('notes').replaceChildren(...notes.map((n) => el('li', '', n)));
  const btn = $('detailsbtn');
  btn.className = 'link' + (notes.length && $('details').hidden ? ' hint' : '');
  btn.textContent = $('details').hidden ? (notes.length ? `Details (${notes.length} note${notes.length > 1 ? 's' : ''})` : 'Details') : 'Hide details';
  // the folder, shown from its end (the part that tells installs apart)
  const found = v.game && !(s && s.found === false);
  $('where').textContent = found ? '‎' + v.game : 'Echo VR not found: pick its folder';
  $('where').title = v.game || '';
  $('change').textContent = found ? 'Change' : 'Find it';
  $('change').disabled = busy;
}

// The tablet mock-up: the player's real tab bar; each tab shows that page with their own data.
const STOCK_PAGES = ['home', 'lobby', 'party', 'friends'];   // the stock tabs, left to right
const PAGE_ICONS = {
  bubble: '<svg viewBox="0 0 40 40"><circle cx="20" cy="20" r="13"/><circle cx="20" cy="20" r="6"/><path d="M8 32L32 8"/></svg>',
  mute: '<svg viewBox="0 0 40 40"><path d="M9 15h5l7-6v16l-7-6H9z"/><path d="M25 12a7 7 0 010 10"/><circle cx="12" cy="31" r="2.5"/><circle cx="20" cy="31" r="2.5"/><circle cx="28" cy="31" r="2.5"/></svg>',
  ghost: '<svg viewBox="0 0 40 40"><circle cx="20" cy="20" r="5"/><path d="M20 6v6M20 28v6M6 20h6M28 20h6M10 10l4 4M26 26l4 4M30 10l-4 4M14 26l-4 4"/></svg>',
  person: '<svg viewBox="0 0 40 40"><circle cx="18" cy="15" r="7"/><path d="M6 33c2-7 7-10 12-10s10 3 12 10"/><path d="M28 26h8M28 30h8"/></svg>',
};
let selected = null;   // index into the preview's tabs; null = PLAY
let launchNote = null;  // the PLAY page's launch button after a press: {text, ok, until}

// The READY spot on the PLAY page opens Echo VR.
function launchState() {
  const s = view && view.scan;
  const running = /running/.test(s && s.problems ? s.problems.join(' ') : '');
  const note = launchNote && Date.now() < launchNote.until ? launchNote : null;
  if (note) return { label: note.ok ? 'STARTING' : 'OOPS', hint: note.text, off: true, bad: !note.ok };
  if (running) return { label: 'RUNNING', hint: 'ECHO VR IS OPEN', off: true };
  if (!view || !view.game || (s && s.found === false)) return { label: 'READY', hint: 'FIND ECHO VR FIRST', off: true };
  return { label: 'READY', hint: 'OPEN ECHO VR', off: false };
}

function launchButton() {
  const st = launchState();
  const b = el('button', 'launchbtn' + (st.off ? ' off' : '') + (st.bad ? ' bad' : ''));
  b.type = 'button';
  b.disabled = st.off;
  b.title = st.off ? st.hint : 'Open Echo VR';
  b.append(el('strong', '', st.label));
  const hint = el('span');
  if (!st.off) hint.append(html('i', '', '<svg viewBox="0 0 24 24"><path d="M7 4.5v15l12-7.5z"/></svg>'));
  hint.append(st.hint);
  b.append(hint);
  b.onclick = launchEcho;
  return b;
}

async function launchEcho() {
  launchNote = { ok: true, text: 'OPENING ECHO VR…', until: Date.now() + 15000 };
  renderTablet(view);
  const r = await api('launch', {});
  launchNote = r.ok ? { ok: true, text: 'ECHO VR IS OPENING', until: Date.now() + 8000 }
                    : { ok: false, text: (r.message || "Couldn't open Echo VR").toUpperCase(), until: Date.now() + 6000 };
  renderTablet(view);
}

// FIND MATCH opens Echo VR too (the search itself is pressed in the game).
function findButton() {
  const st = launchState();
  const b = el('button', 'find' + (st.off ? ' off' : ''));
  b.type = 'button';
  b.disabled = st.off;
  b.title = st.off ? st.hint : 'Open Echo VR';
  b.append(el('b', '', 'FIND MATCH'));
  b.onclick = launchEcho;
  return b;
}

function pageKinds(preview) {
  let stock = 0;
  return preview.map((t) => (t.ours ? 'play' : t.stock ? (STOCK_PAGES[stock++] || 'stock') : 'other'));
}

function html(tag, cls, inner) { const e = el(tag, cls); e.innerHTML = inner; return e; }

function onOff(label, icon, on) {
  const box = el('div', 'hbox');
  box.append(el('small', '', label), html('div', 'hicon' + (on ? ' on' : ''), PAGE_ICONS[icon]),
             el('em', on ? 'on' : '', on == null ? '—' : on ? 'ON' : 'OFF'));
  return box;
}

function peopleList(rows) {
  const list = el('div', 'plist');
  for (const [who, status] of rows) {
    const r = el('div', 'lrow');
    const words = el('div'); words.append(el('b', '', who), el('span', '', status));
    r.append(words, html('div', 'licon', PAGE_ICONS.person));
    list.append(r);
  }
  return list;
}

function message(text) { const m = el('div', 'tmsg'); m.append(el('span', '', text)); return m; }

// The title strip's clock (PLAY only), 24-hour or 12-hour like the one in the game; 24H / 12H flips it.
let clock12 = false;
try { clock12 = localStorage.getItem('clock') === '12'; } catch (e) { /* no storage: 24-hour */ }

function renderClock() {
  const now = new Date();
  const h = now.getHours(), m = String(now.getMinutes()).padStart(2, '0');
  $('tclock').textContent = clock12 ? `${(h + 11) % 12 + 1}:${m} ${h < 12 ? 'AM' : 'PM'}` : `${String(h).padStart(2, '0')}:${m}`;
  $('tfmt').textContent = clock12 ? '12H' : '24H';
}

$('tfmt').onclick = () => {
  clock12 = !clock12;
  try { localStorage.setItem('clock', clock12 ? '12' : '24'); } catch (e) { /* kept for this window only */ }
  renderClock();
};
renderClock();
setInterval(renderClock, 1000);

function renderPage(kind, tab, p) {
  const page = $('tpage');
  page.className = 'page ' + kind;
  $('tclock').hidden = $('tfmt').hidden = kind !== 'play';
  const name = (p && p.name) || 'YOU';
  const heading = (title, sub) => { $('ttitle').textContent = title; $('tsub').textContent = sub || ''; };
  if (kind === 'play') {
    heading('PLAY', 'ARENA MATCHMAKING');
    const card = el('div', 'card');
    const img = el('img', 'mini-disc'); img.src = 'disc.svg'; img.alt = '';
    const words = el('div'); words.append(el('b', '', 'ECHO ARENA'), el('i', '', 'PUBLIC MATCH'));
    card.append(img, words, launchButton());
    // FIND MATCH with CLIP and KEY beside it, then the four boxes, as on the tablet in the game.
    const row = el('div', 'findrow');
    row.append(findButton(), el('span', 'clip', 'CLIP'), el('span', 'clip key', 'KEY'));
    const boxes = el('div', 'boxes');
    for (const [label, value] of [['PRIVATE MATCH', 'CREATE'], ['LOBBY', '⌂ GO'], ['MATCHES OVERALL', p && p.matches != null ? String(p.matches) : '—']]) {
      const b = el('div'); b.append(el('small', '', label), el('span', '', value)); boxes.append(b);
    }
    const gfx = el('div');
    const pair = el('p', 'gfx');
    pair.append(el('span', 'on', 'HIGH'), el('span', '', 'LOW'));
    gfx.append(el('small', '', 'GRAPHICS'), pair);
    boxes.append(gfx);
    page.replaceChildren(card, row, boxes);
  } else if (kind === 'home') {
    heading(name);
    const level = el('div', 'hbox');
    level.append(el('small', '', 'ARENA LEVEL'), el('b', 'level', p && p.level != null ? String(p.level) : '—'));
    const row = el('div', 'hrow');
    row.append(level, onOff('PERSONAL BUBBLE', 'bubble', p && p.bubble), onOff('MUTE ALL', 'mute', p && p.mute), onOff('GHOST ALL', 'ghost', p && p.ghost));
    page.replaceChildren(el('div', 'banner'), row);
  } else if (kind === 'lobby') {
    // Who's in your session, from the game's logs: right now while Echo is open, otherwise the last one.
    const ppl = p && p.people;
    const list = ppl && ppl.lobby.length ? ppl.lobby : [{ name, spectator: false }];
    const when = ppl && ppl.at ? new Date(ppl.at * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : '';
    heading('LOBBY', ppl && ppl.live ? 'RIGHT NOW' : when ? 'LAST SESSION · ' + when : '');
    page.replaceChildren(peopleList(list.map((x) => [x.name, x.spectator ? 'Spectating' : ppl && ppl.live ? 'Online' : 'Was here'])));
  } else if (kind === 'party') {
    heading('PARTY');
    page.replaceChildren(message('Invite players to start a party.'));
  } else if (kind === 'friends') {
    // Echo can't see Meta friends; the people you played with lately stand in.
    const recent = (p && p.people && p.people.recent) || [];
    heading('FRIENDS', recent.length ? 'PLAYED WITH RECENTLY' : '');
    page.replaceChildren(recent.length ? peopleList(recent.map((n) => [n, 'Recent player'])) : message('You have no Oculus Friends.'));
  } else {
    heading((tab && tab.text) || 'MOD', 'FROM ANOTHER MOD');
    page.replaceChildren(message('This tab comes from another mod. It works as usual in Echo VR, next to PLAY.'));
  }
}

function renderTablet(v) {
  const s = v.scan;
  const p = v.player || null;
  const preview = s && s.preview ? s.preview : [];
  const kinds = pageKinds(preview);
  const at = selected != null && selected < preview.length ? selected : Math.max(0, kinds.indexOf('play'));
  const key = preview.map((t) => (t.png || t.text || '').slice(-24)).join('|') + '#' + at + '#' + JSON.stringify(p) + '#' + JSON.stringify(launchState());
  if (key === previewKey) return;
  previewKey = key;
  renderPage(preview.length ? kinds[at] : 'play', preview[at], p);
  const bar = $('tabbar');
  bar.replaceChildren();
  let others = 0;
  preview.forEach((t, i) => {
    const tab = el('button', 't' + (t.ours ? ' ours' : t.stock ? '' : ' other') + (i === at ? ' sel' : ''));
    tab.type = 'button';
    tab.title = { play: 'PLAY', home: 'Home', lobby: 'Lobby', party: 'Party', friends: 'Friends' }[kinds[i]] || 'Another mod';
    if (t.png) { const img = el('img'); img.src = t.png; img.alt = ''; tab.append(img); } else tab.textContent = (t.text || 'TAB').slice(0, 6);
    tab.onclick = () => { selected = i; renderTablet(view); };
    if (!t.ours && !t.stock) others++;
    bar.append(tab);
  });
  $('caption').textContent = (others
    ? `Your tab bar, with PLAY added first. ${others} tab${others > 1 ? 's' : ''} from another mod (dashed) keep${others > 1 ? '' : 's'} working.`
    : 'Your tab bar, with PLAY added first.') + (preview.length ? ' Tap a tab to see your page.' : '');
}

function renderJob(v) {
  const job = v.job;
  const inJob = job.busy;
  const done = !job.busy && showResult && job.ok !== null;
  $('main').hidden = inJob || done;
  $('progress').hidden = !inJob;
  $('result').hidden = !done;
  if (inJob) {
    const stages = STAGES[job.kind] || STAGES.install;
    const at = Math.max(0, stages.findIndex(([k]) => k === job.stage));
    $('ptitle').textContent = job.kind === 'install' ? (situation(v).installed ? 'Repairing EchoLazyTablet' : 'Installing EchoLazyTablet') : 'Removing EchoLazyTablet';
    $('pfill').style.width = `${Math.round(((at + .6) / stages.length) * 100)}%`;
    $('steps').replaceChildren(...stages.map(([, label], i) => el('li', i < at ? 'done' : i === at ? 'now' : '', label)));
  }
  if (done) {
    $('rbadge').className = 'badge' + (job.ok ? '' : ' bad');
    $('rmark').setAttribute('d', job.ok ? 'M15 27l7 7 15-16' : 'M18 18l16 16M34 18L18 34');
    $('rtitle').textContent = job.ok ? (job.kind === 'install' ? 'All set!' : 'Removed') : "That didn't work";
    $('rtext').textContent = job.message;
    $('log').replaceChildren(...job.log.map((l) => el('li', '', l)));
    $('admin').hidden = !job.admin;
  }
}

function render(v) {
  view = v;
  $('version').textContent = 'v' + v.version;
  renderMain(v);
  renderTablet(v);
  renderJob(v);
}

async function refresh() {
  try { render(await api('state')); } catch (e) { /* the installer closed */ }
}

function confirmBox(title, text, yes) {
  return new Promise((resolve) => {
    $('ctitle').textContent = title;
    $('ctext').textContent = text;
    $('cyes').textContent = yes;
    $('confirm').hidden = false;
    const close = (answer) => { $('confirm').hidden = true; resolve(answer); };
    $('cyes').onclick = () => close(true);
    $('cno').onclick = () => close(false);
  });
}

$('install').onclick = async () => {
  if (situation(view).action === 'browse') return render(await api('browse', {}));
  const repair = situation(view).action === 'repair';
  const ok = await confirmBox(repair ? 'Repair EchoLazyTablet?' : 'Install EchoLazyTablet?',
    `${repair ? 'This rebuilds the PLAY tab on your tablet as it is now.' : 'This adds the PLAY tab to your hand tablet.'}\nKeep Echo VR closed until it's done.`,
    repair ? 'Repair' : 'Install');
  if (!ok) return;
  showResult = true;
  render(await api('install', {}));
};
$('uninstall').onclick = async () => {
  if (!(await confirmBox('Uninstall EchoLazyTablet?', 'The PLAY tab and plugin are removed. Your other mods stay exactly as they are.', 'Uninstall'))) return;
  showResult = true;
  render(await api('uninstall', {}));
};
$('detailsbtn').onclick = () => { $('details').hidden = !$('details').hidden; if (view) renderMain(view); };
$('rescan').onclick = async () => render(await api('rescan', {}));
$('browse').onclick = async () => render(await api('browse', {}));
$('change').onclick = async () => render(await api('browse', {}));
$('done').onclick = () => { showResult = false; refresh(); };
$('admin').onclick = () => api('admin', {});

refresh();
setInterval(refresh, 600);
$('bug').onclick = () => api('bug', {});
window.addEventListener('pagehide', () => navigator.sendBeacon('/api/bye?t=' + encodeURIComponent(TOKEN)));
