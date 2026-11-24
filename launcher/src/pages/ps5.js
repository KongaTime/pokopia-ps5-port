import { listen } from '@tauri-apps/api/event';
import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

// PS5 mode: the player's dump folder and console address, then one button that builds the title, makes the shader cache and
// uploads everything to the console (ps5.rs). Each step has its own bar; Pause stops it and Continue picks up where it stopped.

let handler = null;
listen('ps5-status', (ev) => handler && handler(ev.payload));

const STATE_CHIP = {
  done: ['ok', 'circle-check', 'Done'],
  running: ['', 'loader-circle', 'Working'],
  paused: ['warn', 'circle-alert', 'Paused'],
  error: ['danger', 'circle-x', 'Stopped'],
  pending: ['', '', 'To do'],
};

function stepHtml(s, i) {
  const [cls, ic, label] = STATE_CHIP[s.state] || STATE_CHIP.pending;
  const pct = s.total ? Math.min(100, Math.floor((100 * s.done) / s.total)) : 0;
  const bar = s.state === 'done' ? 100 : s.state === 'running' && s.total ? pct : s.state === 'running' ? '' : 0;
  return `
    <div class="candidate ${s.state === 'running' ? 'chosen' : ''}" data-step="${i}">
      <div class="row between">
        <h3>${i + 1}. ${esc(s.label)}</h3>
        <span class="chip ${cls}">${ic ? icon(ic, s.state === 'running' ? 'spin' : '') : ''}${label}${s.state === 'running' && s.total ? ` ${pct}%` : ''}</span>
      </div>
      <progress max="100" ${bar === '' ? '' : `value="${bar}"`}></progress>
      ${s.message ? `<p class="faint">${esc(s.message)}</p>` : ''}
    </div>`;
}

export async function render(el, ctx) {
  const GAME = ctx.game;
  const cfg = await api('get_config');
  // the dump is a folder, or one .nup package (unpacked into <game folder>/dump by the install's first step)
  const isNup = (p) => /\.nup$/i.test(p.trim());
  const savedNup = (cfg.raw_inputs || []).length === 1 && isNup(cfg.raw_inputs[0]) ? cfg.raw_inputs[0] : '';
  let path = savedNup || cfg.dump_dir || '';
  let saved = path;
  let report = null; // dump folder check (validate_dump)
  let nup = null; // {found} or {error} for a .nup (check_raw)
  async function inspect() {
    report = null;
    nup = null;
    if (!path) return;
    if (isNup(path)) {
      try {
        nup = { found: await api('check_raw', { inputs: [path], keys: [] }) };
      } catch (e) {
        nup = { error: errText(e) };
      }
    } else {
      report = await api('validate_dump', { path });
    }
  }
  await inspect();
  let ov = await api('ps5_status');
  let host = ov.host || '';
  let check = { busy: false, text: '', ok: null };
  let finding = false;
  let found = []; // consoles the last search found ({host, installed}), shown as buttons when there are several

  async function find(quiet) {
    finding = true;
    draw();
    try {
      found = await api('ps5_find');
      if (found.length === 1) {
        host = found[0].host;
        check = { busy: false, ok: true, text: `Found your PS5 at ${host}.${found[0].installed ? ' PPSA90006 is already on it; the install updates it.' : ''}` };
      } else if (found.length > 1) {
        check = { busy: false, ok: true, text: `Found ${found.length} PS5s on this network: pick yours below.` };
      } else if (!quiet) {
        check = { busy: false, ok: false, text: 'No PS5 answered on this network. Start the FTP server on the console (port 2121) and make sure this PC is on the same network, or type the address.' };
      }
    } catch (e) {
      if (!quiet) check = { busy: false, ok: false, text: errText(e) };
    }
    finding = false;
    draw();
  }

  function draw() {
    const st = ov.status;
    const running = st.running;
    const ok = (report && report.chosen) || (nup && nup.found);
    const nf = nup && nup.found;
    const started = st.steps.some((s) => s.state !== 'pending');
    const allDone = st.steps.length && st.steps.every((s) => s.state === 'done');
    const label = allDone ? 'Check again' : started ? 'Continue' : 'Install on PS5';
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>Play on your PS5</h1>
          <p class="dim">Puts ${esc(GAME.title)} on your own PS5 as the homebrew title <code>PPSA90006</code>. This PC builds and signs the title from your own dump (the download has no game code), makes the shader cache the console needs, and sends everything to the console over your network. You need a PS5 that runs homebrew and its FTP server (port 2121).</p>
        </div>

        ${ov.available ? '' : `<div class="banner danger">${icon('circle-x')}<div>This download has no PS5 toolchain (<code>tools/ps5</code>). Use a release that includes PS5 mode.</div></div>`}

        <div class="card">
          <h2>${icon('folder-search')}1. Your dump</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="path" spellcheck="false" placeholder="C:/path/to/${esc(GAME.short_title)}_dump" value="${esc(path)}" ${running ? 'disabled' : ''} />
              <button class="btn" id="browse" ${running ? 'disabled' : ''}>${icon('folder-open')}Browse</button>
              <button class="btn" id="pick-nup" ${running ? 'disabled' : ''}>${icon('file-text')}Pick a .nup</button>
            </div>
            <p class="faint">Version ${esc(GAME.dump_version || '2.0.1a')}: the folder with <code>base/</code> and <code>updates/&lt;version&gt;/</code>, each with <code>exefs/</code> and <code>romfs/</code>, or a <code>.nup</code> file with the base game and the update. It is only read; a .nup is unpacked into <code>dump/</code> in the launcher's folder first (about 7 GB).</p>
            ${report && report.errors.length ? `<div class="banner danger">${icon('circle-x')}<div>${report.errors.map(esc).join('<br>')}</div></div>` : ''}
            ${nup && nup.error ? `<div class="banner danger">${icon('circle-x')}<div>${esc(nup.error)}</div></div>` : ''}
            ${nf ? `<p class="dim">${icon('circle-check')} Using the .nup: ${nf.dir === 'base' ? 'the base game' : `update ${esc(String(nf.version))}`}, program build ${esc(nf.build_id.slice(0, 16).toLowerCase())}..., ${nf.files} data files, ${(nf.bytes / 1e9).toFixed(1)} GB${saved === path ? '' : ' (not saved yet)'}</p>` : ''}
            ${nf && nf.notes.length ? `<ul class="checks">${nf.notes.map((n) => `<li>${icon('circle-alert')}${esc(n)}</li>`).join('')}</ul>` : ''}
            ${ok && !nf ? `<p class="dim">${icon('circle-check')} Using ${ok.kind === 'base' ? 'the base game' : `update ${esc(ok.version)}`}: <span class="path">${esc(ok.path)}</span>${saved === path ? '' : ' (not saved yet)'}</p>` : ''}
          </div>
        </div>

        <div class="card">
          <h2>${icon('monitor')}2. Your PS5</h2>
          <div class="stack">
            <div class="row">
              <input type="text" id="host" spellcheck="false" placeholder="192.168.1.20" value="${esc(host)}" ${running ? 'disabled' : ''} style="max-width: 16rem" />
              <button class="btn" id="find" ${running || check.busy || finding ? 'disabled' : ''}>${icon(finding ? 'loader-circle' : 'folder-search', finding ? 'spin' : '')}Find my PS5</button>
              <button class="btn" id="check" ${running || check.busy || finding ? 'disabled' : ''}>${icon(check.busy ? 'loader-circle' : 'hard-drive', check.busy ? 'spin' : '')}Check connection</button>
            </div>
            <p class="faint">Start the FTP server on your PS5 (port 2121), then press Find my PS5. Or type the console's IP address (on the PS5: Settings, Network, Connection status).</p>
            ${found.length > 1 ? `<div class="row">${found.map((f) => `<button class="btn quiet" data-host="${esc(f.host)}">${icon('monitor')}${esc(f.host)}${f.installed ? ' (PPSA90006 installed)' : ''}</button>`).join('')}</div>` : ''}
            ${check.text ? `<div class="banner ${check.ok ? 'ok' : 'danger'}">${icon(check.ok ? 'circle-check' : 'circle-x')}<div>${esc(check.text)}</div></div>` : ''}
          </div>
        </div>

        <div class="card">
          <h2>${icon('wrench')}3. Install</h2>
          <div class="stack">
            <p class="dim">The first time takes hours: the build 10 to 30 minutes, the shader cache one to two hours, and the upload (about 50 GB) depends on your network. You can pause and continue later; finished work is kept, and files already on the console are not sent again. Close the game on the PS5 before installing an update.</p>
            ${st.steps.map(stepHtml).join('')}
            ${st.outcome === 'error' ? `<div class="banner danger">${icon('circle-x')}<div style="white-space: pre-wrap">${esc(st.message)}</div></div>` : ''}
            ${st.outcome === 'done' && !running ? `<div class="banner ok">${icon('circle-check')}<div>Everything is on your PS5. Start <b>PPSA90006</b> from the home screen (the first time, install it with your homebrew installer, as for any fake-signed title). On its start screen pick <b>Prepare game</b> once: it checks the files and shaders. Then <b>Play</b>.</div></div>` : ''}
            <div class="row">
              ${running
                ? `<button class="btn" id="pause">${icon('circle-x')}Pause</button>`
                : `<button class="btn primary big" id="run" ${ov.available && ok && host.trim() ? '' : 'disabled'}>${icon('play')}${label}</button>`}
            </div>
          </div>
        </div>
      </div>`;
    refreshIcons();

    const input = el.querySelector('#path');
    const checkDump = async () => {
      path = input.value.trim();
      await inspect();
      draw();
    };
    input.addEventListener('change', checkDump);
    input.addEventListener('keydown', (e) => e.key === 'Enter' && checkDump());
    el.querySelector('#pick-nup').addEventListener('click', async () => {
      const picked = await api('pick_files', { title: `Choose your ${GAME.short_title} .nup`, filterName: 'NUP packages', extensions: ['nup'], multiple: false });
      if (picked.length) {
        input.value = picked[0];
        checkDump();
      }
    });
    el.querySelector('#browse').addEventListener('click', async () => {
      const picked = await api('pick_folder', { title: `Choose your decrypted ${GAME.short_title} dump folder`, start: input.value || null });
      if (picked) {
        input.value = picked;
        checkDump();
      }
    });
    const hostInput = el.querySelector('#host');
    hostInput.addEventListener('input', () => {
      host = hostInput.value;
      const run = el.querySelector('#run');
      if (run) run.disabled = !(ov.available && ((report && report.chosen) || (nup && nup.found)) && host.trim());
    });
    el.querySelector('#find').addEventListener('click', () => find(false));
    el.querySelectorAll('[data-host]').forEach((b) =>
      b.addEventListener('click', () => {
        host = b.dataset.host;
        found = [];
        check = { busy: false, ok: true, text: `Using the PS5 at ${host}.` };
        draw();
      }),
    );
    el.querySelector('#check').addEventListener('click', async () => {
      check = { busy: true, text: '', ok: null };
      draw();
      try {
        check = { busy: false, text: await api('ps5_check', { host }), ok: true };
      } catch (e) {
        check = { busy: false, text: errText(e), ok: false };
      }
      draw();
    });
    el.querySelector('#run')?.addEventListener('click', async () => {
      try {
        if (saved !== path) {
          if (isNup(path)) await api('save_raw', { inputs: [path], keys: [] });
          else await api('save_dump', { path });
          saved = path;
        }
        await api('ps5_start', { host });
        ov = await api('ps5_status');
        draw();
      } catch (e) {
        toast(errText(e), 'error');
      }
    });
    el.querySelector('#pause')?.addEventListener('click', () => api('ps5_cancel'));
  }

  handler = (s) => {
    const was = ov.status.running;
    ov = { ...ov, status: s };
    // while running only the step cards change, so typing elsewhere is not disturbed
    if (was && s.running) {
      s.steps.forEach((step, i) => {
        const card = el.querySelector(`[data-step="${i}"]`);
        if (card) card.outerHTML = stepHtml(step, i);
      });
      refreshIcons();
      return;
    }
    draw();
  };
  draw();
  // no address saved yet: look for the console right away (quietly: nothing is shown when none answers)
  if (!host.trim() && !ov.status.running) find(true);
}
