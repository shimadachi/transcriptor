const $ = (id) => document.getElementById(id);
const SC = ['--s0','--s1','--s2','--s3','--s4','--s5','--s6','--s7'];
const cssv = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
let prevResult = false, prevSummaryRev = 0;

// Build the VU meter bars once.
const METER_BARS = 40;
(function buildMeter() {
  const m = $('meter');
  for (let i = 0; i < METER_BARS; i++) {
    const b = document.createElement('i');
    b.style.animationDelay = (Math.random() * 0.9).toFixed(2) + 's';
    b.style.animationDuration = (0.7 + Math.random() * 0.6).toFixed(2) + 's';
    m.appendChild(b);
  }
})();

function toast(msg) {
  const t = $('toast'); t.textContent = msg; t.classList.add('on');
  setTimeout(() => t.classList.remove('on'), 2400);
}
async function api(p, o) { return (await fetch(p, o)).json(); }
async function post(p, b) {
  return api(p, {method:'POST', headers:{'Content-Type':'application/json'},
    body: b ? JSON.stringify(b) : null});
}
function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

// ---- minimal, offline Markdown -> HTML (for the summary) ----
// Input is HTML-escaped first, so model output can never inject markup.
function mdEmphasis(s){
  return s
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/~~([^~]+)~~/g, '<del>$1</del>')
    .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
    .replace(/__([^_]+)__/g, '<strong>$1</strong>')
    .replace(/(^|[^*])\*([^*\n]+)\*/g, '$1<em>$2</em>')
    .replace(/(^|[^_])_([^_\n]+)_/g, '$1<em>$2</em>');
}
function mdInline(s){
  // Links come out first and go back in last. A URL is full of the characters
  // the emphasis rules look for -- underscores especially -- so running those
  // over an href would quietly corrupt it.
  const links = [];
  s = s.replace(/\[([^\]\n]*)\]\(([^)\s]+)\)/g, (whole, text, url) => {
    // Model output is untrusted: only ever emit schemes that just navigate,
    // never javascript: or data:.
    if (!/^(https?:\/\/|mailto:)/i.test(url)) return whole;
    return '\u0000L' + (links.push({text, url}) - 1) + '\u0000';
  });
  return mdEmphasis(s).replace(/\u0000L(\d+)\u0000/g, (_, i) => {
    const l = links[+i];
    return `<a href="${l.url}" target="_blank" rel="noopener noreferrer">`
         + `${mdEmphasis(l.text)}</a>`;
  });
}
// A row is a GFM table separator like `| --- | :--: |` (dashes, optional colons).
function isTableSep(line){
  return /^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)+\|?\s*$/.test(line);
}
// Split a `| a | b |` row into trimmed cells, tolerating missing outer pipes.
function splitRow(line){
  let s = line.trim();
  if (s.startsWith('|')) s = s.slice(1);
  if (s.endsWith('|')) s = s.slice(0, -1);
  return s.split('|').map(c => c.trim());
}
function mdTable(header, sep, rows){
  const aligns = splitRow(sep).map(c => {
    const l = c.startsWith(':'), r = c.endsWith(':');
    return l && r ? 'center' : r ? 'right' : l ? 'left' : '';
  });
  const cell = (tag, txt, i) => {
    const a = aligns[i] ? ` style="text-align:${aligns[i]}"` : '';
    return `<${tag}${a}>${mdInline(esc(txt))}</${tag}>`;
  };
  const cols = header.length;
  const pad = (r) => { const c = r.slice(0, cols);
    while (c.length < cols) c.push(''); return c; };
  let h = '<table><thead><tr>';
  pad(header).forEach((c, i) => { h += cell('th', c, i); });
  h += '</tr></thead><tbody>';
  for (const r of rows){ h += '<tr>';
    pad(r).forEach((c, i) => { h += cell('td', c, i); }); h += '</tr>'; }
  return h + '</tbody></table>';
}
function mdToHtml(text){
  const lines = String(text).replace(/\r\n?/g, '\n').split('\n');
  let html = '', list = null, quote = [];
  const closeList = () => { if (list) { html += `</${list}>`; list = null; } };
  const flushQuote = () => { if (quote.length){
    html += `<blockquote>${mdToHtml(quote.join('\n'))}</blockquote>`; quote = []; } };
  for (let i = 0; i < lines.length; i++){
    const line = lines[i].replace(/\s+$/, '');
    // Fenced code block. Checked before everything else: its contents are
    // literal, and a fence full of pipes would otherwise read as a table.
    let fence = /^\s*```+\s*[A-Za-z0-9_+-]*\s*$/.exec(line);
    if (fence){
      closeList(); flushQuote();
      const body = [];
      let j = i + 1;
      for (; j < lines.length && !/^\s*```+\s*$/.test(lines[j]); j++) body.push(lines[j]);
      html += `<pre><code>${esc(body.join('\n'))}</code></pre>`;
      i = j;   // skip the closing fence, or land past the end if unterminated
      continue;
    }
    // GFM pipe table: a header row followed by a separator row.
    if (line.includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1])
        && !isTableSep(line)){
      closeList(); flushQuote();
      const header = splitRow(line); const sep = lines[i + 1];
      const rows = []; let j = i + 2;
      for (; j < lines.length; j++){
        const r = lines[j].replace(/\s+$/, '');
        if (!r.trim() || !r.includes('|')) break;
        rows.push(splitRow(r));
      }
      html += mdTable(header, sep, rows); i = j - 1; continue;
    }
    if (!line.trim()){ closeList(); flushQuote(); continue; }
    // Blockquote: buffer consecutive `>` lines, render recursively.
    let m = /^\s*>\s?(.*)$/.exec(line);
    if (m){ closeList(); quote.push(m[1]); continue; }
    flushQuote();
    // Horizontal rule.
    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)){ closeList(); html += '<hr>'; continue; }
    const e = esc(line);
    m = /^(#{1,4})\s+(.*)$/.exec(e);
    if (m){ closeList(); const lvl = Math.min(m[1].length + 2, 6);
      html += `<h${lvl}>${mdInline(m[2])}</h${lvl}>`; continue; }
    m = /^\s*[-*•–]\s+(.*)$/.exec(e);
    if (m){ if (list !== 'ul'){ closeList(); html += '<ul>'; list = 'ul'; }
      // GFM task list. The models reach for these constantly for action items,
      // and without this the box renders as a literal "[ ]" in front of the text.
      const task = /^\[([ xX])\]\s+(.*)$/.exec(m[1]);
      if (task){
        html += `<li class="task${task[1] === ' ' ? '' : ' done'}">`
              + `${mdInline(task[2])}</li>`;
      } else {
        html += `<li>${mdInline(m[1])}</li>`;
      }
      continue; }
    m = /^\s*\d+[.)]\s+(.*)$/.exec(e);
    if (m){ if (list !== 'ol'){ closeList(); html += '<ol>'; list = 'ol'; }
      html += `<li>${mdInline(m[1])}</li>`; continue; }
    closeList();
    html += `<p>${mdInline(e)}</p>`;
  }
  closeList(); flushQuote();
  return html;
}

// ---- themed custom <select> (replaces the native OS popup) ----
const ICON = {
  out: '<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 9v6h4l5 4V5L7 9H3z"/><path d="M16.5 8.5a5 5 0 0 1 0 7"/></svg>',
  mic: '<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5 11a7 7 0 0 0 14 0"/><line x1="12" y1="18" x2="12" y2="21"/></svg>',
};
function closeAllSelects(){ document.querySelectorAll('.xsel.open').forEach(w => w.classList.remove('open')); }
document.addEventListener('click', closeAllSelects);
document.addEventListener('keydown', e => { if (e.key === 'Escape') closeAllSelects(); });

function enhanceSelect(sel) {
  if (sel._x) return sel._x;
  const wrap = document.createElement('div');
  wrap.className = 'xsel' + (sel.classList.contains('grow') ? ' grow' : '');
  sel.parentNode.insertBefore(wrap, sel);
  wrap.appendChild(sel);
  const btn = document.createElement('button');
  btn.type = 'button'; btn.className = 'xsel-btn';
  const pop = document.createElement('div'); pop.className = 'xsel-pop';
  wrap.appendChild(btn); wrap.appendChild(pop);

  const icon = (opt) => opt.dataset.loop === '1'
    ? `<span class="xsel-ic out">${ICON.out}</span>`
    : opt.dataset.loop === '0' ? `<span class="xsel-ic mic">${ICON.mic}</span>` : '';

  function render() {
    const cur = sel.options[sel.selectedIndex];
    btn.innerHTML = (cur ? icon(cur) : '') +
      `<span class="xsel-label">${cur ? esc(cur.textContent) : ''}</span><span class="xsel-caret"></span>`;
    pop.innerHTML = '';
    Array.from(sel.options).forEach((opt, i) => {
      const row = document.createElement('div');
      row.className = 'xsel-opt' + (i === sel.selectedIndex ? ' sel' : '');
      row.innerHTML = icon(opt) + `<span class="xsel-label">${esc(opt.textContent)}</span>`;
      row.onclick = (e) => {
        e.stopPropagation();
        sel.selectedIndex = i;
        sel.dispatchEvent(new Event('change'));
        render(); wrap.classList.remove('open');
      };
      pop.appendChild(row);
    });
  }
  btn.onclick = (e) => {
    e.stopPropagation();
    if (wrap.classList.contains('disabled')) return;
    const wasOpen = wrap.classList.contains('open');
    closeAllSelects();
    if (!wasOpen) wrap.classList.add('open');
  };
  render();
  sel._x = { render, wrap };
  return sel._x;
}
function refreshSelect(id) { const s = $(id); if (s && s._x) s._x.render(); }
function disableSelect(id, on) {
  const s = $(id); if (!s || !s._x) return;
  s.disabled = on; s._x.wrap.classList.toggle('disabled', on);
  if (on) s._x.wrap.classList.remove('open');
}

// ---- sources ----
// Browser-side capture modes (recorded in the browser, work on Windows too).
const BROWSER_SOURCES = [
  {id: 'browser:both',   key: 'src.browserBoth'},
  {id: 'browser:system', key: 'src.browserSys'},
  {id: 'browser:mic',    key: 'src.browserMic'},
];
let sourcesGen = 0;
async function loadSources() {
  const sel = $('source');
  // Kept across a reload so refreshing (or switching language) does not throw
  // away the source the user picked.
  const prevSrc = sel.value, prevMic = $('micSource').value;
  // Fetch first, touch the DOM only afterwards: start-up and the language pass
  // both load sources, and clearing before the await let the two interleave —
  // each server source ended up listed twice. The generation guard then drops
  // a response that a newer load has already overtaken.
  const gen = ++sourcesGen;
  let d;
  try { d = await api('/api/sources'); } catch { d = {sources: []}; }
  if (gen !== sourcesGen) return;
  sel.innerHTML = '';
  BROWSER_SOURCES.forEach(b => {
    const o = document.createElement('option');
    o.value = b.id; o.textContent = t(b.key);
    sel.appendChild(o);
  });
  const mics = [];
  if (d && d.sources && d.sources.length) {
    // A real server-side source exists (Linux loopback) -> prefer it.
    d.sources.forEach(s => {
      const o = document.createElement('option');
      o.value = s.id; o.textContent = s.label;
      o.dataset.loop = s.is_loopback ? '1' : '0';
      if (s.id === d.default_id) o.selected = true;
      sel.appendChild(o);
      if (!s.is_loopback) mics.push(s);
    });
  } else if (d && d.error) {
    // No host audio (e.g. Windows/container) — browser capture is the way.
    sel.value = 'browser:both';
  }
  // Optional "+ mic" mix dropdown (server-side capture only).
  const msel = $('micSource'); msel.innerHTML = '';
  const none = document.createElement('option');
  none.value = ''; none.textContent = mics.length ? t('src.micNoneAvail') : t('src.micNone');
  msel.appendChild(none);
  mics.forEach(m => {
    const o = document.createElement('option');
    o.value = m.id; o.textContent = '+ ' + m.label;
    msel.appendChild(o);
  });
  const restore = (s, v) => {
    if (v && Array.from(s.options).some(o => o.value === v)) s.value = v;
  };
  restore(sel, prevSrc);
  restore(msel, prevMic);
  refreshSelect('source');
  refreshSelect('micSource');
  updateMicMixVisibility();
}

// The "+ mic" mixer only applies to server-side loopback capture; browser
// sources do their own mic+system mixing client-side.
function updateMicMixVisibility() {
  const v = $('source').value;
  const browser = v.startsWith('browser:');
  const hasMics = $('micSource').options.length > 1;
  const show = !browser && hasMics;
  const wrap = $('micSource')._x && $('micSource')._x.wrap;
  if (wrap) wrap.style.display = show ? '' : 'none';
}

// ---- state poll ----
async function poll() {
  let s;
  try { s = await api('/api/state'); } catch { return; }

  // Merge server-side recording with browser-side (MediaRecorder) capture.
  const rec = browserRec || s.recording;
  const paused = browserRec ? browserPaused : !!s.paused;
  const elapsed = browserRec ? browserElapsed() : s.elapsed;
  const level = browserRec ? browserLevel() : s.level;

  $('badgeText').textContent = (s.device || '').replace(/^[^A-Za-z]+/, '');
  $('led').className = 'led' + (s.cuda ? '' : ' cpu');
  $('statusMsg').textContent = browserRec
    ? (paused ? t('st.browserPaused') : t('st.browserRec'))
    : s.message;
  $('spinner').classList.toggle('on', s.processing);
  $('elapsed').textContent = rec
    ? elapsed.toFixed(1) + 's' + (paused ? t('st.pausedSuffix') : '') : '';

  // meter
  const m = $('meter');
  m.classList.toggle('live', rec && !paused);
  const amp = Math.max(0.28, Math.min(1, level * 11));
  m.style.transform = rec ? `scaleY(${amp})` : 'scaleY(1)';

  // Record button visual. The disc is the whole transport: the dot morphs
  // into a square while recording, and pressing it again stops.
  $('recWrap').classList.toggle('on', rec);
  $('recBtn').title = rec ? t('rec.stop') : t('rec.start');
  $('recBtn').setAttribute('aria-pressed', rec ? 'true' : 'false');

  const busy = rec || s.processing;
  $('recBtn').disabled = s.processing;   // only the models can't be interrupted
  $('pauseBtn').disabled = !rec;
  $('pauseBtn').textContent = paused ? t('src.resume') : t('src.pause');
  $('pauseBtn').classList.toggle('on', paused);
  // Cancel: while recording, or when there's a junk result/error to discard
  // (but not mid-processing — the models can't be interrupted safely).
  $('cancelBtn').disabled = s.processing ||
    !(rec || s.has_audio || s.has_result || s.has_summary || s.phase === 'error');
  disableSelect('source', busy);
  disableSelect('micSource', busy);
  $('refreshSrc').disabled = busy;
  $('fileBtn').disabled = busy;
  // Transcribe stays live after a run: the same audio can go through again
  // with another model, or with speaker separation switched on.
  $('txBtn').disabled = busy || !s.has_audio;
  $('sumBtn').disabled = busy || !s.has_result;

  // Audio is waiting and nothing has been transcribed yet — say so where the
  // transcript will appear.
  if (s.has_audio && !s.has_result && !s.processing) {
    const el = $('transcript');
    const empty = el.querySelector('.empty');
    if (empty) empty.textContent = t('tx.waiting');
  }

  // saved output location
  const saved = $('savedRow');
  if (s.output_dir) { saved.style.display = 'flex'; $('savedPath').textContent = s.output_dir; }
  else saved.style.display = 'none';

  // First-run model notices. Both downloads happen automatically when the
  // pipeline first needs them, so this is informational, not a blocker.
  const hint = $('diarHint');
  const notes = [];
  if (!s.stt_cached) notes.push(t('note.sttMissing'));
  if (s.diarization_enabled && s.diar_supported && !s.diar_cached) {
    notes.push(t('note.diarMissing'));
  }
  if (s.diarization_enabled && !s.diar_supported) notes.push(t('note.diarUnbuilt'));
  if (notes.length) {
    hint.style.display = 'block';
    hint.innerHTML = notes.join('<br>');
  } else hint.style.display = 'none';

  const freshResult  = s.has_result && !prevResult;
  const freshSummary = (s.summary_rev || 0) !== prevSummaryRev;

  if (freshResult) { prevResult = true; await loadResult(); }
  if (!s.has_result) prevResult = false;
  // Re-render on every NEW summary (rev changes), even if one already existed.
  const rev = s.summary_rev || 0;
  if (rev !== prevSummaryRev) { prevSummaryRev = rev; if (s.has_summary) await loadResult(); }

  // A finished run just wrote a folder; refresh the list if it is on screen.
  if ((freshResult || freshSummary) && !$('viewLibrary').hidden) loadLibrary();
}

// ---- results ----
async function loadResult() {
  const d = await api('/api/result');
  renderTranscript(d.result);
  renderSummary(d.summary);
}
function renderTranscript(res, el) {
  el = el || $('transcript');
  if (!res || !res.lines || !res.lines.length) {
    el.innerHTML = '<span class="empty">' + esc(t('tx.none')) + '</span>'; return;
  }
  el.innerHTML = '';
  const idx = {};
  res.lines.forEach(l => {
    const div = document.createElement('div'); div.className = 'line';
    if (l.ts) {
      // Named `ts`, not `t`: `t` is the translation lookup.
      const ts = document.createElement('span');
      ts.className = 'ts'; ts.textContent = l.ts;
      div.appendChild(ts);
    }
    if (res.diarized && l.speaker !== null) {
      div.classList.add('spk');
      if (!(l.speaker in idx)) idx[l.speaker] = Object.keys(idx).length;
      div.style.setProperty('--sc', cssv(SC[idx[l.speaker] % SC.length]));
      const tag = document.createElement('span');
      tag.className = 'spk-tag'; tag.textContent = l.speaker_name;
      div.appendChild(tag);
    }
    const txt = document.createElement('span');
    txt.className = 'txt'; txt.textContent = l.text;
    div.appendChild(txt);
    el.appendChild(div);
  });
}
function renderSummary(text, el, emptyText) {
  el = el || $('summary');
  el.classList.toggle('filled', !!text);
  if (text) el.innerHTML = mdToHtml(text);
  else el.innerHTML = '<span class="empty">' + esc(emptyText || '—') + '</span>';
}

// ---- actions ----
$('recBtn').onclick = async () => {
  // One button for both halves of the transport, so a second press within the
  // 700ms poll window would otherwise read the stale state and start twice.
  // poll() re-enables the button as soon as it has the real answer.
  $('recBtn').disabled = true;
  // Same press stops what it started; poll() keeps .on in sync with the state.
  if (browserRec || $('recWrap').classList.contains('on')) {
    if (browserRec) stopBrowserCapture();
    else post('/api/record/stop');
    return;
  }
  const val = $('source').value;
  if (val.startsWith('browser:')) { startBrowserCapture(val.split(':')[1]); return; }
  const mic = $('micSource').value;
  const body = {source_id: val};
  if (mic) body.mic_source_id = mic;   // mix this mic into the system audio
  const r = await post('/api/record/start', body);
  if (r.error) toast(r.error);
};
$('pauseBtn').onclick = async () => {
  if (browserRec) { toggleBrowserPause(); return; }
  const paused = $('pauseBtn').classList.contains('on');
  await post(paused ? '/api/record/resume' : '/api/record/pause');
};
$('cancelBtn').onclick = async () => {
  if (browserRec) cancelBrowserCapture();   // stop + skip upload
  const r = await post('/api/cancel');       // clear result + delete empty folder
  if (r && r.error) { toast(r.error); return; }
  // Reset the panels to their empty state.
  prevResult = false;
  $('transcript').innerHTML =
    '<span class="empty" data-i18n="tx.empty">' + esc(t('tx.empty')) + '</span>';
  renderSummary(null);
  toast(t('toast.cancelled'));
};
$('fileBtn').onclick = () => $('fileInput').click();
$('fileInput').onchange = async () => {
  const f = $('fileInput').files[0];
  if (!f) return;
  toast(t('toast.uploading') + f.name);
  const fd = new FormData(); fd.append('file', f);
  try {
    const r = await (await fetch('/api/process_file', {method:'POST', body: fd})).json();
    if (r.error) toast(r.error);
  } catch (e) { toast(t('toast.uploadErr')); }
  $('fileInput').value = '';
};
$('txBtn').onclick = async () => {
  const r = await post('/api/transcribe', {});
  if (r.error) toast(r.error);
};
$('sumBtn').onclick = async () => {
  const r = await post('/api/summarize', {
    template: $('tpl').value,
    title: $('ctxTitle').value,
    participants: $('ctxPeople').value,
    notes: $('ctxNotes').value,
  });
  if (r.error) toast(r.error);
};
$('ctxBtn').onclick = () => {
  const box = $('ctxBox');
  const on = box.style.display === 'none';
  box.style.display = on ? 'grid' : 'none';
  $('ctxBtn').classList.toggle('on', on);
};

// Populate the note-template dropdown from settings (labels + saved default).
async function initTpl() {
  try {
    const s = await api('/api/settings');
    const sel = $('tpl'); if (!sel) return;
    sel.innerHTML = '';
    (s.templates || []).forEach(t => {
      const o = document.createElement('option');
      o.value = t.value; o.textContent = t.label;
      if (t.value === s.summary_template) o.selected = true;
      sel.appendChild(o);
    });
    refreshSelect('tpl');
  } catch { /* server not ready yet */ }
}
// Persist the template choice when changed from the summary panel.
$('tpl').addEventListener('change', () => {
  post('/api/settings', {summary_template: $('tpl').value});
});
$('refreshSrc').onclick = loadSources;
$('openFolder').onclick = async () => {
  const r = await post('/api/open_folder');
  if (r.error) toast(r.error); else if (!r.ok) toast(t('toast.folderErr') + r.path);
};

// ---- settings ----
// Template editor state: the built-in prompts from the server, the user's edits
// to them, and the templates the user wrote from scratch (id -> {label, prompt,
// context}). Both kinds are edited through the same three fields.
let tplDefaults = {}, tplOverrides = {}, tplCustom = {}, tplCur = null;

const isCustomTpl = key => Object.prototype.hasOwnProperty.call(tplCustom, key);

// Custom templates get a name field and a delete button; built-ins get a reset.
function syncTplControls(key) {
  const custom = isCustomTpl(key);
  $('s_tplnamefield').style.display = custom ? '' : 'none';
  $('s_tpldel').style.display = custom ? '' : 'none';
  $('s_tplreset').style.display = custom ? 'none' : '';
}
function loadTplEditor(key) {
  if (isCustomTpl(key)) {
    const c = tplCustom[key];
    $('s_tplname').value = c.label || '';
    $('s_tplprompt').value = c.prompt || '';
    $('s_tplctx').value = c.context || '';
  } else {
    const def = tplDefaults[key] || '';
    const ov = tplOverrides[key] || {};
    $('s_tplprompt').value = (ov.prompt && ov.prompt.trim()) ? ov.prompt : def;
    $('s_tplctx').value = ov.context || '';
  }
  syncTplControls(key);
}
function stashTpl(key) {
  if (!key) return;
  const prompt = $('s_tplprompt').value;
  const ctx = $('s_tplctx').value.trim();
  if (isCustomTpl(key)) {
    tplCustom[key] = {
      label: $('s_tplname').value.trim() || t('set.tplUntitled'),
      prompt: prompt.trim(), context: ctx,
    };
    return;
  }
  // Only store a prompt override when it actually differs from the default.
  const def = (tplDefaults[key] || '').trim();
  const promptOv = (prompt.trim() && prompt.trim() !== def) ? prompt.trim() : '';
  if (promptOv || ctx) tplOverrides[key] = {prompt: promptOv, context: ctx};
  else delete tplOverrides[key];
}
function addTplOption(value, label, select) {
  const o = document.createElement('option');
  o.value = value; o.textContent = label;
  (select || $('s_tplsel')).appendChild(o);
}
function setupTplEditor(s) {
  tplDefaults = s.template_defaults || {};
  tplOverrides = JSON.parse(JSON.stringify(s.template_overrides || {}));
  tplCustom = JSON.parse(JSON.stringify(s.custom_templates || {}));
  const sel = $('s_tplsel'); sel.innerHTML = '';
  (s.templates || []).forEach(t => addTplOption(t.value, t.label, sel));
  const known = (s.summary_template in tplDefaults) || isCustomTpl(s.summary_template);
  tplCur = known ? s.summary_template
    : ((s.templates && s.templates[0]) ? s.templates[0].value : 'meeting');
  sel.value = tplCur;
  loadTplEditor(tplCur);
  refreshSelect('s_tplsel');
}
$('s_tplsel').addEventListener('change', () => {
  stashTpl(tplCur); tplCur = $('s_tplsel').value; loadTplEditor(tplCur);
});
$('s_tplreset').onclick = () => { $('s_tplprompt').value = tplDefaults[tplCur] || ''; };

// New template: seeded from whatever is on screen, so it starts from a working
// prompt instead of a blank box. The id is minted once and never changes, so
// renaming it later cannot orphan a saved summary_template.
$('s_tplnew').onclick = () => {
  stashTpl(tplCur);
  const id = 'custom-' + Date.now();
  tplCustom[id] = {
    label: t('set.tplNewName'),
    prompt: $('s_tplprompt').value.trim() || tplDefaults[tplCur] || '',
    context: '',
  };
  addTplOption(id, tplCustom[id].label);
  tplCur = id;
  $('s_tplsel').value = id;
  loadTplEditor(id);
  refreshSelect('s_tplsel');
  $('s_tplname').focus();
  $('s_tplname').select();
};

$('s_tpldel').onclick = () => {
  if (!isCustomTpl(tplCur)) return;
  const gone = tplCur;
  delete tplCustom[gone];
  const sel = $('s_tplsel');
  const opt = Array.from(sel.options).find(o => o.value === gone);
  if (opt) opt.remove();
  tplCur = sel.options.length ? sel.options[0].value : 'meeting';
  sel.value = tplCur;
  loadTplEditor(tplCur);
  refreshSelect('s_tplsel');
};

// Keep the dropdown label in step with the name as it is typed.
$('s_tplname').addEventListener('input', () => {
  if (!isCustomTpl(tplCur)) return;
  const opt = Array.from($('s_tplsel').options).find(o => o.value === tplCur);
  if (opt) opt.textContent = $('s_tplname').value.trim() || t('set.tplUntitled');
  refreshSelect('s_tplsel');
});

// Show only the fields that belong to the selected summarizer backend.
function syncLlmBackend() {
  const embedded = $('s_llmbackend').value !== 'remote';
  // Two pairs: the everyday fields in the Summarizer group, and the matching
  // knobs down in Advanced.
  ['llmEmbedded', 'advLlmEmbedded'].forEach(id => {
    $(id).style.display = embedded ? '' : 'none';
  });
  ['llmRemote', 'advLlmRemote'].forEach(id => {
    $(id).style.display = embedded ? 'none' : '';
  });
}
$('s_llmbackend').addEventListener('change', syncLlmBackend);

// Fill the GGUF dropdown from the paths the server found in the models dir.
function fillGgufList(paths, selected) {
  const sel = $('s_ggufsel');
  sel.innerHTML = '';
  const none = document.createElement('option');
  none.value = ''; none.textContent = paths.length ? t('llm.manualPath') : t('llm.noModel');
  sel.appendChild(none);
  (paths || []).forEach(p => {
    const o = document.createElement('option');
    o.value = p;
    o.textContent = p.split(/[\\/]/).pop();
    if (p === selected) o.selected = true;
    sel.appendChild(o);
  });
  refreshSelect('s_ggufsel');
}
// Picking from the list fills the path box, which is what actually gets saved.
$('s_ggufsel').addEventListener('change', () => {
  if ($('s_ggufsel').value) $('s_llmpath').value = $('s_ggufsel').value;
});
// ---- downloadable summarizer models ----
let llmCatalog = [];
let llmDlTimer = null;

function fillLlmCatalog(catalog) {
  llmCatalog = catalog || [];
  const sel = $('s_llmdl');
  sel.innerHTML = '';
  llmCatalog.forEach(m => {
    const o = document.createElement('option');
    o.value = m.id;
    o.textContent = m.label + ' · ' + m.size + (m.downloaded ? t('llm.downloaded') : '');
    sel.appendChild(o);
  });
  refreshSelect('s_llmdl');
  showLlmNote();
}

function setLlmNote(text, isError) {
  const el = $('llmDlNote');
  el.textContent = text || '';
  el.classList.toggle('err', !!isError);
}

// Default note for the highlighted catalog entry.
function showLlmNote() {
  const m = llmCatalog.find(x => x.id === $('s_llmdl').value);
  if (!m) return setLlmNote('');
  setLlmNote(m.downloaded ? (m.note + t('llm.already'))
                          : (m.note + t('llm.willDl') + m.size));
}
$('s_llmdl').addEventListener('change', showLlmNote);

// Poll until the download thread finishes, then adopt the fetched file.
function pollLlmDownload() {
  if (llmDlTimer) return;
  llmDlTimer = setInterval(async () => {
    let d;
    try { d = await api('/api/llm/download'); } catch (e) { return; }
    if (d.active) {
      const pct = d.progress == null ? '' : ' (%' + Math.round(d.progress * 100) + ')';
      setLlmNote((d.message || t('llm.downloading')) + pct);
      return;
    }
    clearInterval(llmDlTimer); llmDlTimer = null;
    $('dlLlm').disabled = false;
    if (d.error) { setLlmNote(d.error, true); toast(t('toast.dlFailed')); return; }

    setLlmNote(d.message || t('llm.ready'));
    toast(t('toast.dlDone'));
    // The server already pointed the settings at the new file; mirror that here
    // so saving the panel does not undo it.
    const s = await api('/api/settings');
    fillLlmCatalog(s.llm_catalog);
    $('s_llmpath').value = s.llm_model_path || '';
    fillGgufList(s.gguf_models, s.llm_model_path);
  }, 1000);
}

$('dlLlm').onclick = async () => {
  const id = $('s_llmdl').value;
  const m = llmCatalog.find(x => x.id === id);
  if (!m) return;
  if (m.downloaded) {
    // Nothing to fetch — just select it, which is what the user meant.
    $('s_llmpath').value = m.path;
    const s = await api('/api/settings');
    fillGgufList(s.gguf_models, m.path);
    toast(t('toast.dlAlready'));
    return;
  }
  if (!confirm(t('llm.confirmDl', {label: m.label, size: m.size}))) return;

  $('dlLlm').disabled = true;
  setLlmNote(t('llm.starting'));
  const r = await api('/api/llm/download',
                      { method: 'POST', headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ id: id }) });
  if (r.error) {
    $('dlLlm').disabled = false;
    setLlmNote(r.error, true);
    return;
  }
  pollLlmDownload();
};

$('rescanGguf').onclick = async () => {
  const s = await api('/api/settings');
  fillGgufList(s.gguf_models, $('s_llmpath').value);
  toast((s.gguf_models || []).length + t('toast.modelsFound'));
};

async function openSettings() {
  const s = await api('/api/settings');
  $('s_theme').value = s.ui_theme || themePref;
  $('s_device').value = s.device;
  $('s_model').value = s.whisper_model;
  $('s_language').value = s.language;
  $('s_whisperpath').value = s.whisper_model_path || '';
  $('s_sysgain').value = s.system_gain;
  $('s_micgain').value = s.mic_gain;

  $('s_diar').checked = s.enable_diarization;
  $('s_diar').disabled = !s.diar_supported;
  $('s_updates').checked = s.check_updates;
  $('s_version').textContent = s.version || '';
  APP_VERSION = s.version || APP_VERSION;
  APP_REPO = s.repo || APP_REPO;
  $('s_nspk').value = s.num_speakers;
  $('s_clthr').value = s.cluster_threshold;
  $('s_segmodel').value = s.diar_segmentation_model || '';
  $('s_embmodel').value = s.diar_embedding_model || '';

  $('s_outdir').value = s.output_dir;
  $('s_saveaudio').checked = s.save_audio;
  $('s_savetx').checked = s.save_transcript;
  $('s_savesum').checked = s.save_summary;
  $('s_autotx').checked = s.auto_transcribe;
  $('s_autosum').checked = s.auto_summarize;
  $('s_vram').checked = s.manage_vram;

  $('s_llmbackend').value = s.llm_backend || 'embedded';
  $('s_llmpath').value = s.llm_model_path || '';
  $('s_llmctx').value = s.llm_ctx;
  $('s_llmgpu').value = s.llm_gpu_layers;
  $('s_llmmaxtok').value = s.llm_max_tokens;
  fillGgufList(s.gguf_models, s.llm_model_path);
  fillLlmCatalog(s.llm_catalog);
  if (s.llm_download && s.llm_download.active) {
    $('dlLlm').disabled = true;
    pollLlmDownload();
  }
  syncLlmBackend();

  $('s_llmurl').value = s.llm_base_url;
  $('s_llmtimeout').value = s.llm_timeout;
  $('s_sumlang').value = s.summary_language;
  const sel = $('s_llmmodel'); sel.innerHTML = '';
  const o = document.createElement('option');
  o.value = s.llm_model; o.textContent = s.llm_model || t('llm.autoFirst');
  sel.appendChild(o);

  [ 's_modelsdir', 's_modelsdir2' ].forEach(id => {
    if ($(id)) $(id).textContent = s.models_dir || '';
  });

  setupTplEditor(s);
  $('s_uilang').value = s.ui_language || LANG;
  ['s_device','s_model','s_language','s_llmmodel','s_sumlang','s_llmbackend','s_llmdl',
   's_uilang','s_theme'].forEach(refreshSelect);
  showSetTab('general');
  $('modalBg').classList.add('on');
}
// ---- theme (system / light / dark) ----
// Lives in Settings, not in the header. localStorage drives the pre-paint
// render in index.html; config.json is the durable store, so the choice follows
// the app rather than the browser profile. "system" tracks the OS setting live.
let themePref = 'system';
const SYS_LIGHT = window.matchMedia
  ? matchMedia('(prefers-color-scheme: light)') : null;

function resolveTheme(pref) {
  if (pref === 'light' || pref === 'dark') return pref;
  return (SYS_LIGHT && SYS_LIGHT.matches) ? 'light' : 'dark';
}
// The parameter is `pref`, not `t` — `t` is the translation lookup.
function applyTheme(pref, persist) {
  themePref = (pref === 'light' || pref === 'dark') ? pref : 'system';
  document.documentElement.setAttribute('data-theme', resolveTheme(themePref));
  try { localStorage.setItem('transcriptor-theme', themePref); } catch (e) {}
  if (persist) post('/api/settings', {ui_theme: themePref});
}
if (SYS_LIGHT && SYS_LIGHT.addEventListener) {
  SYS_LIGHT.addEventListener('change', () => {
    if (themePref === 'system') applyTheme('system', false);
  });
}

// ---- UI language (en / tr) ----
// Also settings-only. Same two-store arrangement as the theme.
function setLang(lang, persist) {
  applyLang(lang);
  try { localStorage.setItem('transcriptor-lang', LANG); } catch (e) {}
  if ($('s_uilang')) { $('s_uilang').value = LANG; refreshSelect('s_uilang'); }
  if (persist) post('/api/settings', {ui_language: LANG});
}
// Re-render the parts of the UI that JS owns rather than the markup.
window.afterLangChange = () => {
  loadSources(); initTpl(); renderLibraryList(); renderLibraryDetail();
};

// ---- tabs (studio / library) ----
function showTab(name) {
  const lib = name === 'library';
  $('viewStudio').hidden = lib;
  $('viewLibrary').hidden = !lib;
  [['tabStudio', !lib], ['tabLibrary', lib]].forEach(([id, on]) => {
    $(id).classList.toggle('on', on);
    $(id).setAttribute('aria-selected', String(on));
  });
  // Nothing should keep playing behind a hidden tab; the position is kept.
  if (lib) loadLibrary(); else $('libAudio').pause();
}
$('tabStudio').onclick = () => showTab('studio');
$('tabLibrary').onclick = () => showTab('library');

// ---- settings tabs ----
// Same markup and underline as the tabs above; the modal shows one section at
// a time instead of one long scroll. openSettings() resets it to the first.
const SET_TABS = ['general', 'output', 'llm', 'tpl', 'adv'];
function showSetTab(name) {
  SET_TABS.forEach(n => {
    const on = n === name;
    $('setView_' + n).hidden = !on;
    const btn = $('setTab_' + n);
    btn.classList.toggle('on', on);
    btn.setAttribute('aria-selected', String(on));
  });
  // The modal itself is the scroller — a tall tab must not leave the next one
  // scrolled halfway down.
  const modal = document.querySelector('.modal');
  if (modal) modal.scrollTop = 0;
}
SET_TABS.forEach(n => { $('setTab_' + n).onclick = () => showSetTab(n); });

$('settingsBtn').onclick = openSettings;
$('cancelSettings').onclick = () => $('modalBg').classList.remove('on');
$('modalBg').onclick = (e) => { if (e.target === $('modalBg')) $('modalBg').classList.remove('on'); };
$('fetchModels').onclick = async () => {
  const d = await post('/api/llm/models', {llm_base_url: $('s_llmurl').value});
  if (d.error || !d.models.length) { toast(d.error || t('toast.noModel')); return; }
  const sel = $('s_llmmodel'); sel.innerHTML = '';
  d.models.forEach(m => { const o = document.createElement('option'); o.value = m; o.textContent = m; sel.appendChild(o); });
  refreshSelect('s_llmmodel');
};
$('saveSettings').onclick = async () => {
  stashTpl(tplCur);   // capture edits for the currently-open template

  // A custom template with no prompt has nothing to instruct the summarizer
  // with; the server drops those, so say so rather than losing them silently.
  const customs = {};
  let dropped = 0;
  Object.entries(tplCustom).forEach(([id, c]) => {
    if (c.prompt && c.prompt.trim()) customs[id] = c; else dropped++;
  });

  const r = await post('/api/settings', {
    template_overrides: tplOverrides,
    custom_templates: customs,
    device: $('s_device').value, whisper_model: $('s_model').value,
    language: $('s_language').value,
    whisper_model_path: $('s_whisperpath').value,

    enable_diarization: $('s_diar').checked,
    num_speakers: parseInt($('s_nspk').value, 10) || 0,
    cluster_threshold: parseFloat($('s_clthr').value),
    diar_segmentation_model: $('s_segmodel').value,
    diar_embedding_model: $('s_embmodel').value,

    output_dir: $('s_outdir').value, save_audio: $('s_saveaudio').checked,
    save_transcript: $('s_savetx').checked, save_summary: $('s_savesum').checked,
    auto_transcribe: $('s_autotx').checked,
    auto_summarize: $('s_autosum').checked, manage_vram: $('s_vram').checked,
    check_updates: $('s_updates').checked,

    llm_backend: $('s_llmbackend').value,
    llm_model_path: $('s_llmpath').value,
    llm_ctx: parseInt($('s_llmctx').value, 10),
    llm_gpu_layers: parseInt($('s_llmgpu').value, 10),
    llm_max_tokens: parseInt($('s_llmmaxtok').value, 10),
    llm_base_url: $('s_llmurl').value, llm_model: $('s_llmmodel').value,
    llm_timeout: parseFloat($('s_llmtimeout').value),
    summary_language: $('s_sumlang').value,
    ui_language: $('s_uilang').value,
    ui_theme: $('s_theme').value,
    system_gain: parseFloat($('s_sysgain').value),
    mic_gain: parseFloat($('s_micgain').value),
  });
  // Already persisted by the POST above, so only apply them locally.
  if ($('s_uilang').value !== LANG) setLang($('s_uilang').value, false);
  if ($('s_theme').value !== themePref) applyTheme($('s_theme').value, false);
  // Switching the check off takes the banner down now rather than at the next
  // launch; switching it on looks straight away, so the setting visibly does
  // something either way.
  if ($('s_updates').checked) {
    checkUpdates({check_updates: true, version: APP_VERSION, repo: APP_REPO});
  } else {
    $('updBar').style.display = 'none';
  }
  $('modalBg').classList.remove('on');
  if (r.device) { $('badgeText').textContent = r.device.replace(/^[^A-Za-z]+/, ''); }
  initTpl();  // picks up new/renamed templates and any label language change
  toast(dropped ? t('toast.savedDropped', {n: dropped}) : t('toast.saved'));
};

// ===== Browser-side capture (mic + system audio) — records in the browser and
// uploads to the same offline pipeline. Needed on Windows (and any container
// that can't reach host audio). getDisplayMedia/getUserMedia need a secure
// context: use http://localhost or HTTPS.
let browserRec = false, browserPaused = false, _cancelled = false;
let _startTs = 0, _pausedMs = 0, _pauseTs = 0;
let _mr = null, _chunks = [], _streams = [], _actx = null, _analyser = null, _abuf = null;

function browserElapsed() {
  if (!_startTs) return 0;
  const extra = browserPaused ? (performance.now() - _pauseTs) : 0;
  return (performance.now() - _startTs - _pausedMs - extra) / 1000;
}
function browserLevel() {
  if (!_analyser || !_abuf) return 0;
  _analyser.getByteTimeDomainData(_abuf);
  let sum = 0;
  for (let i = 0; i < _abuf.length; i++) { const v = (_abuf[i] - 128) / 128; sum += v * v; }
  return Math.sqrt(sum / _abuf.length);
}
function _pickMime() {
  const cands = ['audio/webm;codecs=opus', 'audio/webm', 'audio/ogg;codecs=opus'];
  for (const t of cands) {
    if (window.MediaRecorder && MediaRecorder.isTypeSupported(t)) return t;
  }
  return '';
}
function _cleanupStreams() {
  _streams.forEach(s => s.getTracks().forEach(t => t.stop()));
  _streams = [];
  if (_actx) { try { _actx.close(); } catch {} _actx = null; }
  _analyser = null; _abuf = null;
}
async function startBrowserCapture(kind) {
  if (browserRec) return;
  if (!navigator.mediaDevices || !window.MediaRecorder) {
    toast(t('toast.noBrowserRec'));
    return;
  }
  _streams = [];
  try {
    if (kind === 'mic' || kind === 'both') {
      _streams.push(await navigator.mediaDevices.getUserMedia(
        {audio: {echoCancellation: false, noiseSuppression: false}}));
    }
    if (kind === 'system' || kind === 'both') {
      const ds = await navigator.mediaDevices.getDisplayMedia({video: true, audio: true});
      ds.getVideoTracks().forEach(t => t.stop());  // we only want the audio
      _streams.push(ds);
    }
  } catch (e) { toast(t('toast.noPermission')); _cleanupStreams(); return; }

  // Mix every captured stream into one track (mic + system together).
  _actx = new (window.AudioContext || window.webkitAudioContext)();
  const dest = _actx.createMediaStreamDestination();
  let hasAudio = false;
  _streams.forEach(s => {
    if (s.getAudioTracks().length) {
      _actx.createMediaStreamSource(s).connect(dest); hasAudio = true;
    }
  });
  if (!hasAudio) {
    toast(t('toast.noAudio'));
    _cleanupStreams(); return;
  }
  _analyser = _actx.createAnalyser(); _analyser.fftSize = 512;
  _abuf = new Uint8Array(_analyser.fftSize);
  _actx.createMediaStreamSource(dest.stream).connect(_analyser);

  _chunks = [];
  const mime = _pickMime();
  _mr = new MediaRecorder(dest.stream, mime ? {mimeType: mime} : undefined);
  _mr.ondataavailable = e => { if (e.data && e.data.size) _chunks.push(e.data); };
  _mr.onstop = onBrowserStop;
  _mr.start(1000);
  browserRec = true; browserPaused = false; _cancelled = false;
  _startTs = performance.now(); _pausedMs = 0; _pauseTs = 0;
  poll();
}
function toggleBrowserPause() {
  if (!_mr) return;
  if (browserPaused) {
    _mr.resume(); _pausedMs += performance.now() - _pauseTs; browserPaused = false;
  } else {
    _mr.pause(); _pauseTs = performance.now(); browserPaused = true;
  }
}
function stopBrowserCapture() {
  if (_mr && _mr.state !== 'inactive') _mr.stop();
}
function cancelBrowserCapture() {
  _cancelled = true;   // onBrowserStop will discard without uploading
  stopBrowserCapture();
}
async function onBrowserStop() {
  browserRec = false; browserPaused = false;
  const type = (_chunks[0] && _chunks[0].type) || 'audio/webm';
  const blob = new Blob(_chunks, {type}); _chunks = [];
  _cleanupStreams();
  if (_cancelled) { _cancelled = false; toast(t('toast.recCancelled')); return; }
  if (!blob.size) { toast(t('toast.emptyRec')); return; }
  const ext = type.includes('ogg') ? 'ogg' : 'webm';
  const fd = new FormData(); fd.append('file', blob, 'recording.' + ext);
  toast(t('toast.processing'));
  try {
    const r = await (await fetch('/api/process_file', {method: 'POST', body: fd})).json();
    if (r.error) toast(r.error);
  } catch (e) { toast(t('toast.uploadErr')); }
}

// ===== Library: past sessions read straight out of the output folder. The
// server does the scanning; nothing is cached here beyond the current listing.
let libSessions = [], libCurrent = null, libItem = null;

// Session ids are "YYYY-MM-DD_HH-MM-SS[_n]"; the folder mtime is the fallback
// for a directory that was not named by us.
function libDate(s) {
  const m = /^(\d{4})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{2})/.exec(s.id);
  const d = m ? new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6])
              : (s.mtime ? new Date(s.mtime * 1000) : null);
  if (!d || isNaN(d.getTime())) return s.id;
  return d.toLocaleString(LANG === 'tr' ? 'tr-TR' : 'en-GB',
    {year: 'numeric', month: 'short', day: 'numeric',
     hour: '2-digit', minute: '2-digit'});
}
function fmtBytes(n) {
  if (!n) return '';
  const u = ['B', 'KB', 'MB', 'GB'];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (v >= 10 || i === 0 ? Math.round(v) : v.toFixed(1)) + ' ' + u[i];
}

async function loadLibrary() {
  $('libList').innerHTML = '<span class="empty">' + esc(t('lib.loading')) + '</span>';
  let d;
  try { d = await api('/api/library'); } catch (e) { d = {sessions: []}; }
  libSessions = d.sessions || [];
  $('libDir').textContent = d.output_dir || '';
  // A recording deleted outside the app leaves the detail pane pointing at
  // nothing; drop the selection rather than showing a stale one.
  if (libCurrent && !libSessions.some(x => x.id === libCurrent)) {
    libCurrent = null; libItem = null;
    $('libAudio').pause();
  }
  renderLibraryList();
  renderLibraryDetail();
}

function renderLibraryList() {
  const list = $('libList');
  if (!list) return;
  list.innerHTML = '';
  if (!libSessions.length) {
    list.innerHTML = '<span class="empty">' + esc(t('lib.none')) + '</span>';
    return;
  }
  libSessions.forEach(s => {
    const tags = [];
    if (s.has_transcript) tags.push(t('lib.badges.tx'));
    if (s.has_summary) tags.push(t('lib.badges.sum'));
    if (s.audio) tags.push(t('lib.badges.aud') + ' · ' + fmtBytes(s.audio_bytes));

    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'lib-row' + (s.id === libCurrent ? ' sel' : '');
    row.innerHTML =
      '<span class="lib-when">' + esc(libDate(s)) + '</span>' +
      '<span class="lib-prev">' + esc(s.preview || '—') + '</span>' +
      '<span class="lib-tags">' +
        tags.map(x => '<i>' + esc(x) + '</i>').join('') + '</span>';
    row.onclick = () => openLibraryItem(s.id);
    list.appendChild(row);
  });
}

async function openLibraryItem(id) {
  let d;
  try { d = await api('/api/library/item?id=' + encodeURIComponent(id)); }
  catch (e) { toast(t('lib.loadErr')); return; }
  if (d.error) { toast(d.error); return; }
  if (libCurrent !== id) $('libAudio').pause();
  libCurrent = id; libItem = d;
  renderLibraryList();     // moves the selection highlight
  renderLibraryDetail();
}

function renderLibraryDetail() {
  if (!$('libBody')) return;
  const d = libItem;
  $('libPlaceholder').hidden = !!d;
  $('libBody').hidden = !d;
  if (!d) return;

  const s = libSessions.find(x => x.id === d.id);
  $('libTitle').textContent = s ? libDate(s) : d.id;
  $('libPath').textContent = d.path || '';

  const audio = $('libAudio');
  if (d.audio) {
    // Only reassign when the recording changed: setting src reloads the stream
    // and would throw away the listening position on a language switch.
    const src = '/api/library/audio?id=' + encodeURIComponent(d.id);
    if (audio.getAttribute('src') !== src) audio.setAttribute('src', src);
    $('libAudioName').textContent = d.audio;
    $('libAudioWrap').hidden = false;
    $('libNoAudio').hidden = true;
  } else {
    audio.pause();
    audio.removeAttribute('src');
    $('libAudioWrap').hidden = true;
    $('libNoAudio').hidden = false;
  }

  // transcript.json keeps the speakers and timestamps, so it renders exactly
  // like the live panel; the .txt is the fallback for older sessions.
  const tx = $('libTranscript');
  if (d.transcript && d.transcript.lines && d.transcript.lines.length) {
    renderTranscript(d.transcript, tx);
  } else if (d.transcript_text) {
    tx.innerHTML = '';
    const raw = document.createElement('div');
    raw.className = 'lib-raw';
    raw.textContent = d.transcript_text;
    tx.appendChild(raw);
  } else {
    tx.innerHTML = '<span class="empty">' + esc(t('lib.noTx')) + '</span>';
  }

  renderSummary(d.summary, $('libSummary'), t('lib.noSum'));
}

$('libRefresh').onclick = loadLibrary;
$('libOpen').onclick = async () => {
  if (!libCurrent) return;
  const r = await post('/api/library/open', {id: libCurrent});
  if (r.error) toast(r.error);
  else if (!r.ok) toast(t('toast.folderErr') + r.path);
};
// Deleting takes the folder off the disk, so it asks first and names what goes.
$('libDelete').onclick = async () => {
  if (!libCurrent || !libItem) return;
  const s = libSessions.find(x => x.id === libCurrent);
  const name = s ? libDate(s) : libCurrent;
  if (!confirm(t('lib.deleteAsk', {name: name, path: libItem.path || libCurrent}))) return;

  const r = await post('/api/library/delete', {id: libCurrent});
  if (r.error) { toast(r.error); return; }
  // Stop playback before the list reload drops the selection — the <audio>
  // still points at a file that is gone.
  $('libAudio').pause();
  $('libAudio').removeAttribute('src');
  libCurrent = null; libItem = null;
  toast(t('lib.deleted'));
  loadLibrary();
};

// ===== update check =====
// The only request this app makes to anything but its own server. GitHub's
// releases API sends Access-Control-Allow-Origin: *, so the page can ask it
// directly — no key, no update service, and nothing about the user goes out
// with the request. Every failure path here is silent: a missed update notice
// is not worth an error in someone's face.
const UPD_TTL = 24 * 60 * 60 * 1000;      // ask GitHub at most once a day
const UPD_SEEN = 'transcriptor-update';       // {ts, tag} — the cached answer
const UPD_HID  = 'transcriptor-update-hid';   // the tag the user dismissed
let APP_VERSION = '', APP_REPO = '';

// Numeric, component by component, so 0.1.10 correctly beats 0.1.9 — a string
// compare has it the other way round. A missing component counts as 0, so
// "0.2" and "0.2.0" are the same version, and a non-numeric suffix on a
// component ("0.2.0-rc1") reads as its leading number.
function cmpVersion(a, b) {
  const pa = String(a).split('.'), pb = String(b).split('.');
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = parseInt(pa[i], 10) || 0, y = parseInt(pb[i], 10) || 0;
    if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}

function updRead(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function updWrite(k, v) { try { localStorage.setItem(k, v); } catch (e) {} }

function showUpdate(tag, current) {
  if (!tag || !current || cmpVersion(tag, current) <= 0) return;
  if (updRead(UPD_HID) === tag) return;          // dismissed, and still the same one
  $('updVer').textContent = tag;
  $('updBar').style.display = 'flex';
}

async function checkUpdates(s) {
  if (!s || !s.version) return;
  APP_VERSION = s.version;
  APP_REPO = s.repo || APP_REPO;
  if (!s.check_updates) return;

  // A cached answer still raises the banner; only the network call is rationed,
  // which also keeps a shared IP well under GitHub's 60-per-hour anonymous cap.
  let seen = null;
  try { seen = JSON.parse(updRead(UPD_SEEN) || 'null'); } catch (e) {}
  if (seen && seen.tag && Date.now() - (seen.ts || 0) < UPD_TTL) {
    showUpdate(seen.tag, s.version);
    return;
  }
  if (!APP_REPO) return;

  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 8000);
  try {
    const r = await fetch('https://api.github.com/repos/' + APP_REPO + '/releases/latest',
                          {headers: {'Accept': 'application/vnd.github+json'},
                           signal: ctl.signal, cache: 'no-store'});
    // 403 rate-limited, 404 no releases yet, 5xx — all just mean "not today".
    if (!r.ok) return;
    // /releases/latest already skips drafts and prereleases. Tags here are bare
    // ("0.1.5"), but tolerate a "v" prefix in case that ever changes.
    const tag = String((await r.json()).tag_name || '').trim().replace(/^v/i, '');
    if (!tag) return;
    // Stamped even when it is not newer, so an up-to-date machine also asks
    // only once a day.
    updWrite(UPD_SEEN, JSON.stringify({ts: Date.now(), tag: tag}));
    showUpdate(tag, s.version);
  } catch (e) {
    /* offline, DNS-blocked, aborted, malformed JSON — all the same: stay quiet */
  } finally {
    clearTimeout(timer);
  }
}

$('updGet').onclick = async () => {
  // The native webview drops target="_blank", so the server hands the URL to
  // the real browser. If that is unreachable, fall back to the page itself.
  try {
    const r = await post('/api/open_releases');
    if (r && !r.ok && r.url) window.open(r.url, '_blank', 'noopener');
  } catch (e) { /* nothing sensible left to try */ }
};
$('updHide').onclick = () => {
  updWrite(UPD_HID, $('updVer').textContent);
  $('updBar').style.display = 'none';
};

// Enhance every native <select> into a themed custom dropdown. Queried rather
// than listed by id: a hand-kept list silently leaves new selects rendering as
// the OS widget, light-on-dark and out of place next to the themed ones.
document.querySelectorAll('select').forEach(enhanceSelect);
$('source').addEventListener('change', updateMicMixVisibility);

loadSources();
initTpl();
// config.json is the durable store: reconcile the pre-paint localStorage guess
// with it, so the choice follows the app rather than the browser profile.
(async function initPrefs() {
  setLang(LANG, false);   // paint from localStorage before the round-trip
  try {
    let saved = localStorage.getItem('transcriptor-theme');
    themePref = (saved === 'light' || saved === 'dark') ? saved : 'system';
  } catch (e) { /* private mode; "system" stands */ }
  try {
    const s = await api('/api/settings');
    if (s.ui_language && s.ui_language !== LANG) setLang(s.ui_language, false);
    if (s.ui_theme && s.ui_theme !== themePref) applyTheme(s.ui_theme, false);
    checkUpdates(s);   // deliberately not awaited: never hold up the first paint
  } catch (e) { /* server not ready yet; the saved values stand */ }
})();
poll();
setInterval(poll, 700);

// Design-preview hooks (only via URL hash).
if (location.hash === '#library') showTab('library');
if (location.hash === '#settings') openSettings();
if (location.hash === '#update') {
  $('updVer').textContent = '9.9.9';
  $('updBar').style.display = 'flex';
}
if (location.hash === '#src') setTimeout(() => { const s = $('source'); if (s && s._x) s._x.wrap.classList.add('open'); }, 300);
if (location.hash === '#demo') {
  renderTranscript({diarized: true, lines: [
    {speaker:0, speaker_name:'Speaker 1', text:'Hello, and welcome to today\'s meeting.'},
    {speaker:1, speaker_name:'Speaker 2', text:'Thanks. Shall we go through the budget lines?'},
    {speaker:0, speaker_name:'Speaker 1', text:'Yes, let\'s start with the marketing spend.'},
    {speaker:2, speaker_name:'Speaker 3', text:'I pulled the figures together; I\'ll share them shortly.'},
  ]});
  renderSummary('• Summary: The meeting covered the budget and marketing spend; figures are to follow.\n• Key Points:\n  – The marketing budget will be reviewed\n  – Speaker 3 will share the figures\n• Action Items:\n  – Speaker 1 will pull the deck together');
}
