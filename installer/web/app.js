// EchoQueue installer window. Talks to webapp.py; every call carries the token from the URL.
const TOKEN = location.hash.slice(1);
const $ = (id) => document.getElementById(id);
let view = null;
let showResult = false;
let previewKey = '';

const ICONS = {
  ok: '<svg viewBox="0 0 24 24"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>',
  warn: '<svg viewBox="0 0 24 24"><path d="M12 4l9 16H3z"/><path d="M12 10v4M12 17.5v.01"/></svg>',
  bad: '<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg>',
  install: '<path d="M12 3v12m0 0l-5-5m5 5l5-5M4 19h16" />',
  repair: '<path d="M14.7 6.3a4 4 0 00-5.4 5.4L3 18l3 3 6.3-6.3a4 4 0 005.4-5.4l-2.6 2.6-2.4-.6-.6-2.4z" />',
};
const STAGES = {
  install: [['check', 'Checking your game'], ['build', 'Building your PLAY tab'], ['write', 'Installing the tab'],
            ['verify', 'Double-checking'], ['plugin', 'Adding the EchoQueue plugin']],
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
  if (!s || s.found === false) return { kind: 'bad', title: "Can't find Echo VR", text: 'Open Details and pick the folder called ready-at-dawn-echo-arena.' };
  const p = s.problems.join(' ');
  if (/build/.test(p)) return { kind: 'bad', title: 'Different Echo VR version', text: 'EchoQueue is made for Echo VR build 1683152886.' };
  if (/EchoLoader/.test(p)) return { kind: 'bad', title: 'EchoLoader needed', text: 'Install EchoLoader first: EchoQueue runs as one of its plugins.' };
  if (s.problems.some((x) => !/running/.test(x))) return { kind: 'bad', title: "Can't add the tab", text: s.problems.filter((x) => !/running/.test(x)).join('\n') };
  const running = /running/.test(p);
  if (s.state.includes('Repair')) return { kind: 'warn', title: 'Needs a quick repair', text: running ? 'Close Echo VR, then repair.' : 'Another mod changed your tablet. Repair puts PLAY back.', action: 'repair' };
  if (s.state.startsWith('installed')) return { kind: 'ok', title: "You're all set", text: running ? 'EchoQueue is installed. Close Echo VR to make changes.' : `EchoQueue ${v.version} is installed. Repair if your tab ever goes missing.`, action: 'repair', installed: true };
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
    pill(!/build/.test(p), 'Right version');
    pill(!/EchoLoader/.test(p), 'EchoLoader');
    pill(!/running/.test(p), /running/.test(p) ? 'Close Echo VR' : 'Echo closed');
  }

  const busy = v.job.busy || v.scanning;
  const blocked = !sit.action || /running/.test(s ? s.problems.join(' ') : '');
  $('install').disabled = busy || blocked;
  $('ctatext').textContent = sit.action === 'repair' ? (sit.installed ? 'Repair EchoQueue' : 'Repair now') : 'Install EchoQueue';
  $('ctaicon').innerHTML = sit.action === 'repair' ? ICONS.repair : ICONS.install;
  $('uninstall').hidden = !(s && (s.state.startsWith('installed') || s.state.includes('Repair')));
  $('uninstall').disabled = busy;
  $('rescan').disabled = busy;

  $('path').textContent = v.game || 'Not found';
  $('path').title = v.game || '';
  const mods = s && s.found !== false ? [...(s.echomods ? ['Echo Mods'] : []), ...(s.plugins || []).map((m) => m.replace(/\.dll$/i, ''))] : [];
  $('modsfield').hidden = !mods.length;
  $('mods').replaceChildren(...mods.map((m) => el('span', '', m)));
  const notes = s && s.found !== false ? [...(s.echomods ? ['Echo Mods found: EchoQueue is switched on in its profiles, so Echo Mods keeps it on.'] : []), ...(s.notes || [])] : [];
  $('notes').replaceChildren(...notes.map((n) => el('li', '', n)));
  const btn = $('detailsbtn');
  btn.className = 'link' + (notes.length && $('details').hidden ? ' hint' : '');
  btn.textContent = $('details').hidden ? (notes.length ? `${notes.length} note${notes.length > 1 ? 's' : ''} about your mods` : 'Details') : 'Hide details';
}

function renderTablet(v) {
  const s = v.scan;
  const key = s && s.preview ? s.preview.map((t) => (t.png || t.text || '').slice(-24)).join('|') : '';
  if (key === previewKey) return;
  previewKey = key;
  const bar = $('tabbar');
  bar.replaceChildren();
  if (!s || !s.preview) return;
  let others = 0;
  for (const t of s.preview) {
    const tab = el('div', 't' + (t.ours ? ' ours' : t.stock ? '' : ' other'));
    if (t.png) { const img = el('img'); img.src = t.png; img.alt = ''; tab.append(img); } else tab.textContent = (t.text || 'TAB').slice(0, 6);
    if (!t.ours && !t.stock) others++;
    bar.append(tab);
  }
  $('caption').textContent = others
    ? `Your tab bar, with PLAY added first. ${others} tab${others > 1 ? 's' : ''} from another mod (dashed) keep${others > 1 ? '' : 's'} working.`
    : 'Your tab bar, with PLAY added first.';
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
    $('ptitle').textContent = job.kind === 'install' ? (situation(v).installed ? 'Repairing EchoQueue' : 'Installing EchoQueue') : 'Removing EchoQueue';
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
  const repair = situation(view).action === 'repair';
  const ok = await confirmBox(repair ? 'Repair EchoQueue?' : 'Install EchoQueue?',
    `${repair ? 'This rebuilds the PLAY tab on your tablet as it is now.' : 'This adds the PLAY tab to your hand tablet.'}\nKeep Echo VR closed until it's done.`,
    repair ? 'Repair' : 'Install');
  if (!ok) return;
  showResult = true;
  render(await api('install', {}));
};
$('uninstall').onclick = async () => {
  if (!(await confirmBox('Uninstall EchoQueue?', 'The PLAY tab and plugin are removed. Your other mods stay exactly as they are.', 'Uninstall'))) return;
  showResult = true;
  render(await api('uninstall', {}));
};
$('detailsbtn').onclick = () => { $('details').hidden = !$('details').hidden; if (view) renderMain(view); };
$('rescan').onclick = async () => render(await api('rescan', {}));
$('browse').onclick = async () => render(await api('browse', {}));
$('done').onclick = () => { showResult = false; refresh(); };
$('admin').onclick = () => api('admin', {});

refresh();
setInterval(refresh, 600);
