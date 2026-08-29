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
function mdInline(s){
  return s
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
    .replace(/__([^_]+)__/g, '<strong>$1</strong>')
    .replace(/(^|[^*])\*([^*\n]+)\*/g, '$1<em>$2</em>')
    .replace(/(^|[^_])_([^_\n]+)_/g, '$1<em>$2</em>');
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
      html += `<li>${mdInline(m[1])}</li>`; continue; }
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
async function loadSources() {
  const sel = $('source');
  // Kept across a reload so refreshing (or switching language) does not throw
  // away the source the user picked.
  const prevSrc = sel.value, prevMic = $('micSource').value;
  sel.innerHTML = '';
  BROWSER_SOURCES.forEach(b => {
    const o = document.createElement('option');
    o.value = b.id; o.textContent = t(b.key);
    sel.appendChild(o);
  });
  let d;
  try { d = await api('/api/sources'); } catch { d = {sources: []}; }
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

  // record button visual
  $('recWrap').classList.toggle('on', rec);
  $('recBtn').title = rec ? t('rec.recording') : t('rec.start');

  const busy = rec || s.processing;
  $('recBtn').disabled = busy;
  $('stopBtn').disabled = !rec;
  $('pauseBtn').disabled = !rec;
  $('pauseBtn').textContent = paused ? t('src.resume') : t('src.pause');
  $('pauseBtn').classList.toggle('on', paused);
  // Cancel: while recording, or when there's a junk result/error to discard
  // (but not mid-processing — the models can't be interrupted safely).
  $('cancelBtn').disabled = s.processing ||
    !(rec || s.has_result || s.has_summary || s.phase === 'error');
  disableSelect('source', busy);
  disableSelect('micSource', busy);
  $('refreshSrc').disabled = busy;
  $('fileBtn').disabled = busy;
  $('sumBtn').disabled = busy || !s.has_result;

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

  if (s.has_result && !prevResult) { prevResult = true; await loadResult(); }
  if (!s.has_result) prevResult = false;
  // Re-render on every NEW summary (rev changes), even if one already existed.
  const rev = s.summary_rev || 0;
  if (rev !== prevSummaryRev) { prevSummaryRev = rev; if (s.has_summary) await loadResult(); }
}

// ---- results ----
async function loadResult() {
  const d = await api('/api/result');
  renderTranscript(d.result);
  renderSummary(d.summary);
}
function renderTranscript(res) {
  const el = $('transcript');
  if (!res || !res.lines.length) {
    el.innerHTML = '<span class="empty" data-i18n="tx.none">' + esc(t('tx.none')) + '</span>'; return;
  }
  el.innerHTML = '';
  const idx = {};
  res.lines.forEach(l => {
    const div = document.createElement('div'); div.className = 'line';
    if (l.ts) {
      const t = document.createElement('span');
      t.className = 'ts'; t.textContent = l.ts;
      div.appendChild(t);
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
function renderSummary(text) {
  const el = $('summary');
  el.classList.toggle('filled', !!text);
  if (text) el.innerHTML = mdToHtml(text);
  else el.innerHTML = '<span class="empty">—</span>';
}

// ---- actions ----
$('recBtn').onclick = async () => {
  const val = $('source').value;
  if (val.startsWith('browser:')) { startBrowserCapture(val.split(':')[1]); return; }
  const mic = $('micSource').value;
  const body = {source_id: val};
  if (mic) body.mic_source_id = mic;   // mix this mic into the system audio
  const r = await post('/api/record/start', body);
  if (r.error) toast(r.error);
};
$('stopBtn').onclick = () => {
  if (browserRec) { stopBrowserCapture(); return; }
  post('/api/record/stop');
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
  $('llmEmbedded').style.display = embedded ? '' : 'none';
  $('llmRemote').style.display = embedded ? 'none' : '';
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
    o.textContent = m.label + ' · ' + m.size + (m.downloaded ? ' · inik' : '');
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
                          : (m.note + ' — indirilecek: ' + m.size));
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
  if (!confirm(m.label + ' indirilecek (' + m.size + '). Devam edilsin mi?')) return;

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
  $('s_device').value = s.device;
  $('s_model').value = s.whisper_model;
  $('s_language').value = s.language;
  $('s_whisperpath').value = s.whisper_model_path || '';
  $('s_sysgain').value = s.system_gain;
  $('s_micgain').value = s.mic_gain;

  $('s_diar').checked = s.enable_diarization;
  $('s_diar').disabled = !s.diar_supported;
  $('s_nspk').value = s.num_speakers;
  $('s_clthr').value = s.cluster_threshold;
  $('s_segmodel').value = s.diar_segmentation_model || '';
  $('s_embmodel').value = s.diar_embedding_model || '';

  $('s_outdir').value = s.output_dir;
  $('s_saveaudio').checked = s.save_audio;
  $('s_savetx').checked = s.save_transcript;
  $('s_savesum').checked = s.save_summary;
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
  o.value = s.llm_model; o.textContent = s.llm_model || '(otomatik: ilk model)';
  sel.appendChild(o);

  [ 's_modelsdir', 's_modelsdir2' ].forEach(id => {
    if ($(id)) $(id).textContent = s.models_dir || '';
  });

  setupTplEditor(s);
  $('s_uilang').value = s.ui_language || LANG;
  ['s_device','s_model','s_language','s_llmmodel','s_sumlang','s_llmbackend','s_llmdl',
   's_uilang'].forEach(refreshSelect);
  $('modalBg').classList.add('on');
}
// ---- theme (dark / light) ----
// The parameter is `mode`, not `t` — `t` is the translation lookup.
function applyTheme(mode) {
  document.documentElement.setAttribute('data-theme', mode);
  try { localStorage.setItem('transcriptor-theme', mode); } catch (e) {}
  // Icon shows the mode you can switch TO.
  $('themeBtn').textContent = mode === 'light' ? '☾' : '☀';
}
$('themeBtn').textContent =
  document.documentElement.getAttribute('data-theme') === 'light' ? '☾' : '☀';
$('themeBtn').onclick = () => {
  const cur = document.documentElement.getAttribute('data-theme') === 'light'
    ? 'light' : 'dark';
  applyTheme(cur === 'light' ? 'dark' : 'light');
};

// ---- UI language (tr / en) ----
// localStorage drives the pre-paint render in i18n.js; config.json is the
// durable store, so every change is persisted to both.
function setLang(lang, persist) {
  applyLang(lang);
  try { localStorage.setItem('transcriptor-lang', LANG); } catch (e) {}
  // The button shows the language you can switch TO, like the theme icon.
  $('langBtn').textContent = LANG === 'en' ? 'TR' : 'EN';
  if ($('s_uilang')) { $('s_uilang').value = LANG; refreshSelect('s_uilang'); }
  if (persist) post('/api/settings', {ui_language: LANG});
}
// Re-render the parts of the UI that JS owns rather than the markup.
window.afterLangChange = () => { loadSources(); initTpl(); };
$('langBtn').onclick = () => setLang(LANG === 'en' ? 'tr' : 'en', true);

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
    auto_summarize: $('s_autosum').checked, manage_vram: $('s_vram').checked,

    llm_backend: $('s_llmbackend').value,
    llm_model_path: $('s_llmpath').value,
    llm_ctx: parseInt($('s_llmctx').value, 10),
    llm_gpu_layers: parseInt($('s_llmgpu').value, 10),
    llm_max_tokens: parseInt($('s_llmmaxtok').value, 10),
    llm_base_url: $('s_llmurl').value, llm_model: $('s_llmmodel').value,
    llm_timeout: parseFloat($('s_llmtimeout').value),
    summary_language: $('s_sumlang').value,
    ui_language: $('s_uilang').value,
    system_gain: parseFloat($('s_sysgain').value),
    mic_gain: parseFloat($('s_micgain').value),
  });
  // Already persisted by the POST above, so only apply it locally.
  if ($('s_uilang').value !== LANG) setLang($('s_uilang').value, false);
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

// Enhance every native <select> into a themed custom dropdown. Queried rather
// than listed by id: a hand-kept list silently leaves new selects rendering as
// the OS widget, light-on-dark and out of place next to the themed ones.
document.querySelectorAll('select').forEach(enhanceSelect);
$('source').addEventListener('change', updateMicMixVisibility);

loadSources();
initTpl();
// config.json is the durable store: reconcile the pre-paint localStorage guess
// with it, so the choice follows the app rather than the browser profile.
(async function initLang() {
  setLang(LANG, false);   // paint the button before the round-trip
  try {
    const s = await api('/api/settings');
    if (s.ui_language && s.ui_language !== LANG) setLang(s.ui_language, false);
  } catch (e) { /* server not ready yet; the saved value stands */ }
})();
poll();
setInterval(poll, 700);

// Design-preview hooks (only via URL hash).
if (location.hash === '#settings') openSettings();
if (location.hash === '#src') setTimeout(() => { const s = $('source'); if (s && s._x) s._x.wrap.classList.add('open'); }, 300);
if (location.hash === '#demo') {
  renderTranscript({diarized: true, lines: [
    {speaker:0, speaker_name:'Konuşmacı 1', text:'Merhaba, bugünkü toplantıya hoş geldiniz.'},
    {speaker:1, speaker_name:'Konuşmacı 2', text:'Teşekkürler. Bütçe kalemlerini konuşalım mı?'},
    {speaker:0, speaker_name:'Konuşmacı 1', text:'Evet, önce pazarlama giderlerinden başlayalım.'},
    {speaker:2, speaker_name:'Konuşmacı 3', text:'Rakamları ben derledim, birazdan paylaşırım.'},
  ]});
  renderSummary('• Özet: Toplantıda bütçe ve pazarlama giderleri ele alındı; rakamlar hazırlanacak.\n• Ana Noktalar:\n  – Pazarlama bütçesi gözden geçirilecek\n  – Konuşmacı 3 rakamları paylaşacak\n• Aksiyonlar:\n  – Konuşmacı 1 sunumu toparlayacak');
}
