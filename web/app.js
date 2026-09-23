const $ = (id) => document.getElementById(id);
const SC = ['--s0','--s1','--s2','--s3','--s4','--s5','--s6','--s7'];
const cssv = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
let prevResultRev = 0, prevSummaryRev = 0;
// Whether the studio transcript actually holds words. has_result only says a
// run finished: a take of silence finishes with a result whose lines are empty
// (or absent altogether), and summarizing that asks a model to load and think
// about nothing, to arrive at "There is no text to summarize."
let txHasText = false;

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

// This run's authorization, stamped into the page by the server. Every request
// that changes something carries it: loopback is not a boundary on its own, so
// without it any page in the browser could drive this app behind the user's
// back. A custom header also forces a CORS preflight on anything cross-origin,
// which nothing here answers.
const CSRF = (document.querySelector('meta[name="csrf-token"]') || {}).content || '';
function authHeaders(extra) {
  return Object.assign({'X-Transcriptor-Token': CSRF}, extra || {});
}
async function post(p, b) {
  return api(p, {method:'POST', headers: authHeaders({'Content-Type':'application/json'}),
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
// A list item, or null: how deep it sits, which kind of list it belongs to,
// the number the model gave it, and its text. Takes the line already escaped,
// which leaves the markers and the indentation as they were.
function mdListItem(e){
  const m = /^(\s*)(?:([-*•–])|(\d+)[.)])\s+(.*)$/.exec(e);
  if (!m) return null;
  return {indent: m[1].replace(/\t/g, '    ').length, tag: m[2] ? 'ul' : 'ol',
          num: m[3] ? parseInt(m[3], 10) : 1, text: m[4]};
}
function mdToHtml(text){
  const lines = String(text).replace(/\r\n?/g, '\n').split('\n');
  let html = '', quote = [];
  // Open lists, outermost first, each with its last <li> still open so that a
  // deeper list can go inside it. A numbered section with its bullets indented
  // under it is the shape models write most, and a flat renderer closed the
  // <ol> at the first indented bullet: every section restarted at "1." and its
  // bullets fell out of it.
  const lists = [];
  const closeTo = (indent) => {
    while (lists.length && lists[lists.length - 1].indent > indent) {
      html += `</li></${lists.pop().tag}>`;
    }
  };
  const closeList = () => closeTo(-1);
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
    if (!line.trim()){
      // A blank line between items is a loose list, not the end of one: keep
      // it open when what follows still belongs to it.
      let j = i + 1;
      while (j < lines.length && !lines[j].trim()) j++;
      const next = j < lines.length ? lines[j] : '';
      if (!(lists.length && (mdListItem(esc(next)) || /^\s+\S/.test(next)))) closeList();
      flushQuote(); continue;
    }
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
    const item = mdListItem(e);
    if (item){
      closeTo(item.indent);
      const top = lists[lists.length - 1];
      if (top && top.indent === item.indent && top.tag !== item.tag){
        html += `</li></${lists.pop().tag}>`;    // same depth, other kind of list
      }
      const cur = lists[lists.length - 1];
      if (cur && cur.indent === item.indent){
        html += '</li>';
      } else {
        // `start` keeps the model's own number, so a list a paragraph broke
        // into carries on counting instead of beginning again at 1.
        const start = item.tag === 'ol' && item.num !== 1 ? ` start="${item.num}"` : '';
        html += `<${item.tag}${start}>`;
        lists.push({tag: item.tag, indent: item.indent});
      }
      // GFM task list. The models reach for these constantly for action items,
      // and without this the box renders as a literal "[ ]" in front of the text.
      const task = /^\[([ xX])\]\s+(.*)$/.exec(item.text);
      html += task ? `<li class="task${task[1] === ' ' ? '' : ' done'}">${mdInline(task[2])}`
                   : `<li>${mdInline(item.text)}`;
      continue;
    }
    // Text indented under an item belongs to it -- the explanation models put
    // beneath a numbered heading -- rather than ending the list.
    if (lists.length && line.match(/^\s*/)[0].replace(/\t/g, '    ').length >
                        lists[lists.length - 1].indent){
      html += `<br>${mdInline(e.trim())}`;
      continue;
    }
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

// ---- the page's own confirm ----
// window.confirm() is rendered by the webview host rather than by the page: it
// drops in at the top of the window in the system's own colours, and no
// stylesheet here can touch it. This is the same question asked in the app's
// language. Returns a promise, so callers read the way they did before.
let askResolve = null;
function ask(opts) {
  return new Promise(resolve => {
    askClose(false);            // never leave one hanging behind another
    askResolve = resolve;
    $('askTitle').textContent = opts.title || '';
    $('askBody').textContent  = opts.body || '';
    $('askYes').textContent   = opts.yes || t('ask.yes');
    $('askNo').textContent    = opts.no || t('set.cancel');
    $('askBg').classList.add('on');
    $('askYes').focus();
  });
}
function askClose(answer) {
  if (!askResolve) return;
  const resolve = askResolve;
  askResolve = null;
  $('askBg').classList.remove('on');
  resolve(answer);
}
$('askYes').onclick = () => askClose(true);
$('askNo').onclick  = () => askClose(false);
// Clicking the backdrop is a decline, the same as Cancel — and the same as the
// settings modal, so the two dialogs do not disagree about what a stray click
// outside them means.
$('askBg').onclick = (e) => { if (e.target === $('askBg')) askClose(false); };
document.addEventListener('keydown', (e) => {
  if (!askResolve) return;
  if (e.key === 'Escape') { e.preventDefault(); askClose(false); }
  else if (e.key === 'Enter') {
    // Enter answers with the button that has focus. Treating every Enter as
    // "yes" turned Enter on a focused Cancel into the deletion it was
    // declining. Anywhere else the key is swallowed: nothing behind the dialog
    // may be pressed while it is up.
    e.preventDefault();
    const f = document.activeElement;
    if (f === $('askYes')) askClose(true);
    else if (f === $('askNo')) askClose(false);
  }
});
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
    // Every option is laid into the same grid cell as the value, so the
    // trigger is as wide as the longest name rather than the chosen one.
    // Sized by the selection it shrank when you picked a short template, and
    // the popup — which is the width of the trigger — then ellipsised the very
    // names it is there to show.
    const ghosts = Array.from(sel.options).map(o =>
      `<span class="xsel-ghost" aria-hidden="true">${esc(o.textContent)}</span>`).join('');
    btn.innerHTML = (cur ? icon(cur) : '') +
      `<span class="xsel-value"><span class="xsel-label">${cur ? esc(cur.textContent) : ''}</span>` +
      `${ghosts}</span><span class="xsel-caret"></span>`;
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
  // The label in the markup names the native select, which is hidden. Hand
  // it to the button that stands in for it, so clicking the label opens the
  // list and a screen reader hears "Interface language, English".
  if (sel.id) {
    btn.id = sel.id + '_btn';
    document.querySelectorAll('label[for="' + sel.id + '"]').forEach(l => {
      l.htmlFor = btn.id;
      if (!l.id) l.id = sel.id + '_lbl';
      btn.setAttribute('aria-labelledby', l.id + ' ' + btn.id);
    });
  }
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
  renderProgress('statusPct', 'statusTrack', 'statusFill', jobFraction(s));
  renderLibStatus(s);

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

  const busy = rec || s.processing || browserStarting || browserUploading;
  // Only the models can't be interrupted — and a capture that is still asking
  // for permission, which has nothing to stop yet, or one whose take is still
  // on its way to the server.
  $('recBtn').disabled = s.processing || browserStarting || browserUploading;
  $('pauseBtn').disabled = !rec;
  $('pauseBtn').textContent = paused ? t('src.resume') : t('src.pause');
  $('pauseBtn').classList.toggle('on', paused);
  // Cancel: while a job runs it stops the models; otherwise it discards the
  // take or result on screen. The engines have always been able to give up
  // mid-run — it is what closing the window does — so the only thing missing
  // was a way to ask them.
  $('cancelBtn').disabled = browserUploading ||
    !(s.processing || rec || s.has_audio || s.has_result || s.has_summary ||
      s.phase === 'error');
  $('cancelBtn').title = s.processing ? t('btn.cancelJob') : '';
  disableSelect('source', busy);
  disableSelect('micSource', busy);
  $('refreshSrc').disabled = busy;
  $('fileBtn').disabled = busy;
  // Transcribe stays live after a run — the same audio can go through again
  // with another model, or with speaker separation switched on — but not
  // without the weights on disk: pressing it used to start a job whose first
  // minutes were a silent 3 GB download.
  $('txBtn').disabled = busy || !s.has_audio || !s.stt_cached;
  $('txBtn').title = s.stt_cached ? '' : t('note.sttBtn');
  syncSumBtn(s, busy);

  // saved output location
  const saved = $('savedRow');
  if (s.output_dir) { saved.style.display = 'flex'; $('savedPath').textContent = s.output_dir; }
  else saved.style.display = 'none';
  // A folder that exists is not a take that was saved. With a write failed,
  // "Saved →" beside "Not saved →" contradicted itself about the same folder,
  // so the row only says where the folder is.
  $('savedK').textContent = t(s.save_error ? 'saved.folder' : 'saved.k');

  // …and what could not be written. Stays up until the next take: a recording
  // that only exists in memory is one the user loses by closing the window, so
  // this must not be a toast that scrolls away unseen.
  const serr = $('saveErr');
  if (s.save_error) { serr.style.display = 'flex'; $('saveErrMsg').textContent = s.save_error; }
  else serr.style.display = 'none';

  // First-run model notices, both of them a way in rather than a line of text
  // to read and wonder what to do with. The speech one opens the panel that
  // picks a model, because there is a choice to make; the speaker models have
  // no choice attached, so that one just fetches them and reports here.
  const hint = $('diarHint');
  const notes = [];
  const dl = s.model_download || {};
  if (!s.stt_cached) {
    notes.push('<a href="#settings" class="caution" data-go="stt">' +
               t('note.sttMissing') + '</a>');
  }
  if (s.diarization_enabled && s.diar_supported && !s.diar_cached) {
    if (dl.active && dl.kind === 'diarize') {
      const pct = dl.progress == null ? '' : pctText(dl.progress);
      notes.push('<span class="caution busy">' +
                 esc(dl.message || t('llm.downloading')) + pct + '</span>');
    } else if (dl.kind === 'diarize' && dl.error && !dl.cancelled) {
      notes.push('<a class="caution err" data-go="diar">' + esc(dl.error) +
                 t('note.diarRetry') + '</a>');
    } else {
      notes.push('<a class="caution" data-go="diar">' + t('note.diarMissing') + '</a>');
    }
  }
  if (s.diarization_enabled && !s.diar_supported) notes.push(t('note.diarUnbuilt'));
  if (notes.length) {
    hint.style.display = 'block';
    hint.innerHTML = notes.join('<br>');
  } else hint.style.display = 'none';

  // Watch the revision, not has_result. Transcribing the same audio again never
  // lowers that flag, so a run-to-run change was invisible and the panel kept
  // showing the previous text until a summary happened to land.
  //
  // The revision now moves when a result is *cleared* as well — starting a new
  // recording or upload bumps it — and reloading is no longer conditional on
  // there being something to load. Skipping the reload when has_result was
  // false is what left the last session's transcript and summary on screen
  // beside the new session's audio, indefinitely with auto-transcribe off.
  const resRev = s.result_rev || 0, sumRev = s.summary_rev || 0;
  const freshResult  = resRev !== prevResultRev;
  const freshSummary = sumRev !== prevSummaryRev;

  if (freshResult || freshSummary) {
    // Commit the revisions only once the panels have actually been rendered.
    // Committing first meant a single failed /api/result — a blip, a restart —
    // left the stale transcript on screen for good: every later poll then
    // compared equal and never tried the reload again.
    try {
      // renders the empty state too, so a cleared panel clears
      if (await loadResult()) {
        prevResultRev = resRev; prevSummaryRev = sumRev;
        // The load is what learns whether there are words, and it happens after
        // the buttons were set above; without this the Summarize button spent a
        // whole tick describing the previous transcript.
        syncSumBtn(s, busy);
      }
    } catch (e) { /* leave the revisions be; the next poll retries */ }
  }

  // Nothing transcribed yet: say what the panel is waiting for. Runs after the
  // reload above, so a panel cleared on this same tick gets the right line
  // instead of the library's "no transcript found".
  if (!s.has_result && !s.processing) {
    const empty = $('transcript').querySelector('.empty');
    if (empty) empty.textContent = s.has_audio ? t('tx.waiting') : t('tx.empty');
  }

  // A finished run just wrote a folder; refresh the list if it is on screen.
  if ((freshResult || freshSummary) && !$('viewLibrary').hidden) loadLibrary();
}

// ---- results ----
// Returns whether this call rendered. One fetch at a time: each poll tick is
// 700ms and a fetch can outlast that, so several can be in the air at once.
//
// Discarding the superseded responses instead — a generation counter — starved
// the panels outright whenever responses were consistently slower than the
// interval: every response was already stale by the time it arrived, nothing
// rendered, no revision was ever committed, and each tick launched yet another
// request. Declining to start a second one has the same protection against
// out-of-order renders and cannot starve: the caller leaves the revision
// uncommitted, so the next tick simply tries again.
let resultLoading = false;
async function loadResult() {
  if (resultLoading) return false;
  resultLoading = true;
  try {
    const d = await api('/api/result');
    // Read before rendering, and only here: renderTranscript() also draws the
    // library's panel, which must not move the studio's button.
    txHasText = transcriptHasText(d.result);
    renderTranscript(d.result);
    renderSummary(d.summary);
    return true;
  } finally {
    resultLoading = false;
  }
}
// Nothing to summarize without words, whatever has_result says. Says which of
// the two it is on hover, so a dimmed button is not a dead end — and is read
// fresh each time, so the reason follows a language switch.
function syncSumBtn(s, busy) {
  const noWords = !s.has_result || !txHasText;
  $('sumBtn').disabled = busy || noWords;
  $('sumBtn').title = noWords ? t(s.has_result ? 'sum.emptyTx' : 'sum.noTx') : '';
}

// A whitespace-only line is not text: whisper hands back a timed, speaker-
// tagged line with nothing in it often enough that counting lines would call an
// empty transcript full.
function transcriptHasText(res) {
  return !!(res && res.lines &&
            res.lines.some(l => l && typeof l.text === 'string' && l.text.trim()));
}

// `seekable` turns the timestamps into controls that move the playback head.
// Only the library passes it, and only when the recording kept its audio:
// there is nothing to jump into otherwise, and a timestamp that looks pressable
// and does nothing is worse than one that never offered.
function renderTranscript(res, el, seekable) {
  el = el || $('transcript');
  // Only the library panel is followed along with the audio, so only its lines
  // go into the index. Keyed on the element rather than on `seekable`, because
  // the studio panel redraws on every poll and would otherwise wipe an index
  // built for the recording open in the other tab.
  const indexed = (el === $('libTranscript'));
  if (indexed) plIndexReset();
  if (!res || !res.lines || !res.lines.length) {
    el.innerHTML = '<span class="empty">' + esc(t('tx.none')) + '</span>'; return;
  }
  el.innerHTML = '';
  const idx = {};
  res.lines.forEach(l => {
    const div = document.createElement('div'); div.className = 'line';
    if (indexed && seekable && typeof l.start === 'number') {
      plLines.push({start: l.start,
                    end: typeof l.end === 'number' ? l.end : l.start,
                    el: div});
    }
    if (l.ts) {
      // Named `ts`, not `t`: `t` is the translation lookup.
      const ts = document.createElement('span');
      ts.className = 'ts'; ts.textContent = l.ts;
      if (seekable && typeof l.start === 'number') {
        // data-at is what the delegated handler and the CSS both key off, so
        // the two can never disagree about which stamps are live.
        ts.dataset.at = l.start;
        ts.setAttribute('role', 'button');
        ts.setAttribute('tabindex', '0');
        ts.title = t('lib.seekTo');
      }
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
// Cancel is three actions wearing one button, and each gives up something
// different. A single "are you sure?" would be worth nothing here, so the
// question names what this particular press is about to throw away.
const CANCEL_ASK = {
  job:     {title: 'ask.cancelJobTitle',     body: 'ask.cancelJob',
            yes:   'ask.cancelJobYes',       no:   'ask.cancelJobNo'},
  rec:     {title: 'ask.discardTitle',       body: 'ask.cancelRec',
            yes:   'ask.discard',            no:   'ask.cancelRecNo'},
  discard: {title: 'ask.cancelDiscardTitle', body: 'ask.cancelDiscard',
            yes:   'ask.discard',            no:   'ask.cancelDiscardNo'},
};

$('cancelBtn').onclick = async () => {
  // Which of the three this is decides the question, so read the state now
  // rather than trusting whatever the last poll left on the screen.
  let s = {};
  try { s = await api('/api/state'); } catch { /* ask anyway, below */ }
  const rec = browserRec || s.recording;
  const kind = rec ? 'rec' : (s.processing ? 'job' : 'discard');

  // An error banner over an empty studio is the one press with nothing to
  // lose. Asking there would train the answer out of people.
  if (rec || s.processing || s.has_audio || s.has_result || s.has_summary) {
    const q = CANCEL_ASK[kind];
    const go = await ask({title: t(q.title), body: t(q.body),
                          yes:   t(q.yes),   no:   t(q.no)});
    if (!go) return;
    // A run can finish while the question is on screen, and /api/cancel on a
    // finished run discards its result instead of stopping anything. Agreeing
    // to stop a job is not agreeing to throw away the transcript it produced.
    if (kind === 'job') {
      let now = {};
      try { now = await api('/api/state'); } catch { return; }
      if (!now.processing) { toast(t('toast.jobAlreadyDone')); poll(); return; }
    }
  }

  if (browserRec) cancelBrowserCapture();   // stop + skip upload
  const r = await post('/api/cancel');
  if (r && r.error) { toast(r.error); return; }
  // Stopping a run throws nothing away, so the panels stay as they are — the
  // transcript a cancelled summary was reading is still the transcript.
  if (r && r.stopped === 'job') { toast(t('toast.jobStopped')); return; }
  // A cancelled take or a discarded result does clear them. The revision is
  // left alone: it only ever climbs, so the next run's transcript still reads
  // as new.
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
    const r = await (await fetch('/api/process_file',
      {method:'POST', headers: authHeaders(), body: fd})).json();
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
// The GPUs ggml reports, named the way the badge names them. Built fresh every
// time Settings opens: an eGPU can come and go between two visits, and a stale
// id would otherwise sit there pointing at nothing.
function fillDeviceList(devices, selected) {
  const sel = $('s_device');
  sel.innerHTML = '';
  const mk = (value, label) => {
    const o = document.createElement('option');
    o.value = value; o.textContent = label;
    if (value === selected) o.selected = true;
    sel.appendChild(o);
  };
  mk('auto', t('set.auto'));
  (devices || []).forEach(d => {
    const gb = d.vram ? ' · ' + (d.vram / 1073741824).toFixed(1) + ' GB' : '';
    const kind = d.integrated ? ' · ' + t('set.integrated') : '';
    mk(d.id, d.name + ' (' + d.backend + ')' + gb + kind);
  });
  mk('cpu', 'CPU');
  // A card named in the settings but missing now: keep it visible rather than
  // silently snapping to Automatic, so it is clear what the app is doing.
  if (selected && selected !== 'auto' && selected !== 'cpu' &&
      !(devices || []).some(d => d.id === selected)) {
    mk(selected, selected + ' · ' + t('set.deviceGone'));
    sel.value = selected;
  }
  refreshSelect('s_device');
}

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
// ---- downloadable models ----
// Speech weights and summarizer GGUFs behave identically from here: pick from
// a catalog that says what each one is and what it costs, press Download, watch
// it land. The server runs one download at a time, so one timer serves both.
//
// The speech catalog is the model picker itself, not an optional shortcut
// beside it — nothing is fetched during a transcription any more, so this panel
// is the only place weights ever arrive from.
const DL = {
  llm:     {sel: 's_llmdl', btn: 'dlLlm',     cancel: 'cancelLlmDl',     note: 'llmDlNote'},
  whisper: {sel: 's_model', btn: 'dlWhisper', cancel: 'cancelWhisperDl', note: 'whisperDlNote'},
};
let catalogs = {llm: [], whisper: []};
let dlTimer = null;
let dlKind = 'llm';       // which section the running download belongs to

function catalogEntry(kind, id) {
  return catalogs[kind].find(x => x.id === id);
}

// The sign leads in Turkish and trails in English. It was hardcoded the
// Turkish way, which was quiet enough inside the settings panel and much less
// so once a download started reporting itself on the main screen.
function pctText(frac) {
  const n = Math.round(frac * 100);
  return LANG === 'tr' ? ' (%' + n + ')' : ' (' + n + '%)';
}

// How far along the running job is, or null where the stage cannot say. Both
// stages that can — the transcription, and a local summarizer working through
// its sections or its answer budget — report a fraction on /api/state; a
// remote summarizer sends none, and gets no number rather than a made-up one.
function jobFraction(s) {
  return s.processing && typeof s.progress === 'number' ? s.progress : null;
}

// One readout, drawn twice: the studio console and the library strip show the
// same job from the same poll.
function renderProgress(pctId, trackId, fillId, frac) {
  $(pctId).textContent = frac == null ? '' : pctText(frac);
  $(trackId).hidden = frac == null;
  if (frac != null) $(fillId).style.width = Math.round(frac * 100) + '%';
}

// `placeholder` heads the list with an unselected entry, so "no model yet" is a
// state the dropdown can actually show rather than a silent first item.
//
// `stored` is what the settings currently hold. A dropdown that cannot show its
// own saved value reports "" for it, and the next save writes that "" back —
// so a name the catalog does not list (a legacy large-v2, or a model put there
// by hand) is added as an option of its own. fillDeviceList() keeps a missing
// card visible for the same reason.
function fillCatalog(kind, list, placeholder, stored) {
  catalogs[kind] = list || [];
  const sel = $(DL[kind].sel);
  const keep = sel.value;
  sel.innerHTML = '';
  if (placeholder) {
    const o = document.createElement('option');
    o.value = ''; o.textContent = placeholder;
    sel.appendChild(o);
  }
  catalogs[kind].forEach(m => {
    const o = document.createElement('option');
    o.value = m.id;
    o.textContent = m.label + ' · ' + m.size + (m.downloaded ? t('llm.downloaded') : '');
    sel.appendChild(o);
  });
  if (stored && !catalogEntry(kind, stored)) {
    const o = document.createElement('option');
    o.value = stored;
    o.textContent = stored + t('stt.notListed');
    sel.appendChild(o);
  }
  // Whatever was on screen wins a refill, so rebuilding the list under an open
  // panel does not move the user's choice; the stored value is what a first
  // fill lands on.
  const has = (v) => Array.from(sel.options).some(o => o.value === v);
  if (keep && has(keep)) sel.value = keep;
  else if (stored && has(stored)) sel.value = stored;
  refreshSelect(DL[kind].sel);
  showNote(kind);
}

function setNote(kind, text, isError) {
  const el = $(DL[kind].note);
  el.textContent = text || '';
  el.classList.toggle('err', !!isError);
}

// Default note for the highlighted catalog entry: what it is, then whether it
// is here or what fetching it will cost.
function showNote(kind) {
  const m = catalogEntry(kind, $(DL[kind].sel).value);
  if (!m) return setNote(kind, kind === 'whisper' ? t('stt.pick') : '');
  setNote(kind, m.downloaded ? (m.note + t('llm.already'))
                             : (m.note + t('llm.willDl') + m.size));
}
$('s_llmdl').addEventListener('change', () => showNote('llm'));
$('s_model').addEventListener('change', () => showNote('whisper'));

// Download and Cancel are the same control in two states: only one of them is
// ever useful, so only one is ever on screen.
function setDlActive(kind, on) {
  $(DL[kind].btn).disabled = on;
  const c = $(DL[kind].cancel);
  c.style.display = on ? '' : 'none';
  if (on) c.disabled = false;
}

// Poll until the download thread finishes, then adopt the fetched file.
function pollDownload() {
  if (dlTimer) return;
  dlTimer = setInterval(async () => {
    let d;
    try { d = await api('/api/model/download'); } catch (e) { return; }
    const kind = d.kind || dlKind;
    // The speaker models are fetched from the studio caution and report
    // themselves there; this panel has no controls for them to drive.
    if (!DL[kind]) {
      if (!d.active) { clearInterval(dlTimer); dlTimer = null; }
      return;
    }
    if (d.active) {
      const pct = d.progress == null ? '' : pctText(d.progress);
      setNote(kind, (d.message || t('llm.downloading')) + pct);
      setDlActive(kind, true);
      return;
    }
    clearInterval(dlTimer); dlTimer = null;
    setDlActive(kind, false);
    if (d.error) {
      // A download the user stopped is not a failure: same note, not in red.
      setNote(kind, d.error, !d.cancelled);
      toast(d.cancelled ? t('toast.dlCancelled') : t('toast.dlFailed'));
      return;
    }

    setNote(kind, d.message || t('llm.ready'));
    toast(t('toast.dlDone'));
    // The server already pointed the settings at the new file; mirror that here
    // so saving the panel does not undo it.
    const s = await api('/api/settings');
    if (kind === 'whisper') {
      fillCatalog('whisper', s.whisper_catalog, t('stt.none'), s.whisper_model);
      $('s_model').value = s.whisper_model || '';
      $('s_whisperpath').value = s.whisper_model_path || '';
      refreshSelect('s_model');
    } else {
      fillCatalog('llm', s.llm_catalog);
      $('s_llmpath').value = s.llm_model_path || '';
      fillGgufList(s.gguf_models, s.llm_model_path);
    }
  }, 1000);
}

async function startDownload(kind) {
  const id = $(DL[kind].sel).value;
  const m = catalogEntry(kind, id);
  if (!m) return;
  if (m.downloaded) {
    // Nothing to fetch — just select it, which is what the user meant.
    if (kind === 'whisper') {
      $('s_whisperpath').value = '';   // the picked model, not a stale path
    } else {
      $('s_llmpath').value = m.path;
      const s = await api('/api/settings');
      fillGgufList(s.gguf_models, m.path);
    }
    toast(t('toast.dlAlready'));
    return;
  }
  const go = await ask({
    title: t('ask.downloadTitle'),
    body:  t('llm.confirmDl', {label: m.label, size: m.size}),
    yes:   t('set.llmDlBtn'),
  });
  if (!go) return;

  dlKind = kind;
  setDlActive(kind, true);
  setNote(kind, t('llm.starting'));
  // post(), not a hand-built header set. Building the headers here omitted
  // X-Transcriptor-Token, so the server answered every download with 403
  // "This page is out of date — reload it" — which reloading could not fix,
  // because the header was never being sent in the first place.
  const r = await post('/api/model/download', {kind: kind, id: id});
  if (r.error) {
    setDlActive(kind, false);   // both controls back, not just Download
    setNote(kind, r.error, true);
    return;
  }
  pollDownload();
}

async function cancelDownload(kind) {
  // Ask, then let the poll above tell the story: the download thread kills its
  // curl, drops the .part file and goes inactive, which resets both buttons.
  $(DL[kind].cancel).disabled = true;
  setNote(kind, t('llm.cancelling'));
  try { await post('/api/model/download/cancel'); } catch (e) { /* poll recovers */ }
}

// ---- transcript → playhead ----
// Clicking a timestamp in a saved transcript moves the recording to it. The
// stamps are rebuilt whenever a recording is opened, so the listeners are
// delegated to the panel rather than attached per line.
function seekLibraryTo(sec) {
  const a = $('libAudio');
  if (!a.getAttribute('src') || !isFinite(sec)) return;

  const jump = () => {
    const dur = a.duration;
    a.currentTime = Math.max(0, isFinite(dur) && dur > 0 ? Math.min(sec, dur) : sec);
    plPaint();
    // Jumping to a moment is a request to hear it. Already playing, it just
    // moves; paused, it starts — this is a click, so autoplay allows it.
    if (a.paused) { const p = a.play(); if (p && p.catch) p.catch(() => {}); }
  };

  // preload="metadata" usually has the length in hand already; on the very
  // first click of a fresh page it may not, and seeking before then is ignored.
  if (a.readyState > 0) jump();
  else a.addEventListener('loadedmetadata', jump, {once: true});
}

function stampAt(target) {
  const el = target && target.closest && target.closest('.ts[data-at]');
  return el ? parseFloat(el.dataset.at) : NaN;
}

$('libTranscript').addEventListener('click', (e) => {
  const at = stampAt(e.target);
  if (!isNaN(at)) seekLibraryTo(at);
});
$('libTranscript').addEventListener('keydown', (e) => {
  if (e.key !== 'Enter' && e.key !== ' ') return;
  const at = stampAt(e.target);
  if (isNaN(at)) return;
  e.preventDefault();
  seekLibraryTo(at);
});
// Reading ahead of the playhead has to be possible. These three are the
// reader's own scrolling and nothing else -- listening for 'scroll' instead
// would also catch plFollow's own, and the panel would freeze itself.
['wheel', 'touchmove', 'pointerdown'].forEach(ev => {
  $('libTranscript').addEventListener(ev, () => { plScrolledAt = Date.now(); },
                                      {passive: true});
});

$('dlLlm').onclick     = () => startDownload('llm');
$('dlWhisper').onclick = () => startDownload('whisper');
$('cancelLlmDl').onclick     = () => cancelDownload('llm');
$('cancelWhisperDl').onclick = () => cancelDownload('whisper');

$('rescanGguf').onclick = async () => {
  const s = await api('/api/settings');
  fillGgufList(s.gguf_models, $('s_llmpath').value);
  toast((s.gguf_models || []).length + t('toast.modelsFound'));
};

// The clustering threshold is not a setting any more: it is derived from the
// transcription language by Settings::cluster_threshold in src/config.cpp. The
// panel still says which value that is, and follows the language select live
// rather than waiting for a save and a reopen.
const CLTHR_BY_LANG = {tr: 0.80, en: 0.65, '': 0.70};

function showClthrNote() {
  const note = $('clthrNote');
  if (!note) return;
  const value = CLTHR_BY_LANG[$('s_language').value];
  note.textContent = t('set.clthr')
    .replace('{v}', (value === undefined ? CLTHR_BY_LANG[''] : value).toFixed(2));
}

async function openSettings() {
  const s = await api('/api/settings');
  $('s_theme').value = s.ui_theme || themePref;
  fillDeviceList(s.devices, s.device);
  // Built from the catalog, so the sizes and the "already here" marks are
  // current every time the panel opens. The placeholder is what a fresh
  // install lands on: nothing is chosen until the user chooses.
  fillCatalog('whisper', s.whisper_catalog, t('stt.none'), s.whisper_model);
  $('s_model').value = s.whisper_model || '';
  refreshSelect('s_model');
  showNote('whisper');
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
  showClthrNote();
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
  $('s_llmthink').checked = !!s.llm_thinking;
  fillGgufList(s.gguf_models, s.llm_model_path);
  fillCatalog('llm', s.llm_catalog);
  // A download that is already running belongs to whichever section started
  // it, so the panel picks its controls back up where they were left.
  if (s.model_download && s.model_download.active && DL[s.model_download.kind]) {
    dlKind = s.model_download.kind;
    setDlActive(dlKind, true);
    pollDownload();
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
  // Assigned, not added: openSettings() runs on every open, and addEventListener
  // would stack another copy of the handler each time.
  $('s_language').onchange = showClthrNote;
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
  showClthrNote();   // written by JS, so the i18n sweep does not reach it
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
  // The body is the scroller — a tall tab must not leave the next one
  // scrolled halfway down.
  const body = $('setBody');
  if (body) body.scrollTop = 0;
}
SET_TABS.forEach(n => { $('setTab_' + n).onclick = () => showSetTab(n); });

$('settingsBtn').onclick = openSettings;

// The first-run cautions are the way to act on themselves. Delegated, because
// the notice is rebuilt from scratch on every poll that needs it.
$('diarHint').addEventListener('click', async (e) => {
  const link = e.target.closest && e.target.closest('a.caution');
  if (!link) return;
  e.preventDefault();

  // The speaker models have nothing to choose between, so there is nothing to
  // go to Settings for: fetch them here and let the notice carry the progress.
  // The next poll picks the state up from /api/state and redraws the line.
  if (link.dataset.go === 'diar') {
    const r = await post('/api/model/download', {kind: 'diarize', id: 'diarize'});
    if (r.error) toast(r.error);
    return;
  }

  await openSettings();
  showSetTab('general');
  // Land on the control, not just the tab it lives in.
  const sel = $('s_model');
  if (sel && sel._x && sel._x.wrap && sel._x.wrap.scrollIntoView) {
    sel._x.wrap.scrollIntoView({block: 'center'});
  }
});
$('cancelSettings').onclick = () => $('modalBg').classList.remove('on');
$('modalBg').onclick = (e) => { if (e.target === $('modalBg')) $('modalBg').classList.remove('on'); };
$('fetchModels').onclick = async () => {
  // Send the backend along with the URL. Without it the server fell back to the
  // saved one, so picking "Remote server" and pressing Fetch before saving
  // listed the .gguf files on disk and never contacted the server at all.
  const d = await post('/api/llm/models', {llm_backend: $('s_llmbackend').value,
                                           llm_base_url: $('s_llmurl').value});
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
    llm_thinking: $('s_llmthink').checked,
    llm_base_url: $('s_llmurl').value, llm_model: $('s_llmmodel').value,
    llm_timeout: parseFloat($('s_llmtimeout').value),
    summary_language: $('s_sumlang').value,
    ui_language: $('s_uilang').value,
    ui_theme: $('s_theme').value,
    system_gain: parseFloat($('s_sysgain').value),
    mic_gain: parseFloat($('s_micgain').value),
  });
  // Nothing was persisted if the server said no — it refuses while a job runs.
  // Closing the modal and reporting "Settings saved" regardless threw away every
  // edit in it, template prompts included, with nothing on screen to say so.
  if (r.error) { toast(r.error); return; }

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
// browserStarting covers the gap between the click and the permission answer.
// browserRec cannot: it is only true once the user has granted, and poll() puts
// the Record button back within 700ms because the *server* is idle — so a
// second click during the prompt started a second capture over the first.
// browserUploading covers the handoff after the capture is torn down but before
// the server has the take: browserRec is already false there, so polling would
// otherwise re-enable Record and let a second take start on top of the upload.
let browserRec = false, browserStarting = false, browserPaused = false, _cancelled = false;
let browserUploading = false;
// Whether the current take has already been handed off. Both the recorder's
// stop event and the error fallback can arrive; only the first may act.
let _finalized = false;
// Which take the recorder callbacks belong to. _finalized alone is per-page and
// is reset by every new capture, so a fallback timer left over from an errored
// take found it false again and finished the take after it: the new recording
// was cut off mid-sentence, its partial chunks uploaded as a whole take, and
// the rest lost because the real stop event then found the work already done.
let _takeId = 0, _stopTimer = null;
// How long the error fallback waits for a stop event that may never come.
let _stopFallbackMs = 2000;
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
  if (browserRec || browserStarting || browserUploading) return;
  // A retained take is the only copy of a recording. Starting another one would
  // hand the slot to the new take and quietly drop the old, so make the user
  // resolve it first — retry, save a copy, or discard.
  if (_pendingBlob) { toast(t('toast.resolvePending')); return; }
  if (!navigator.mediaDevices || !window.MediaRecorder) {
    toast(t('toast.noBrowserRec'));
    return;
  }
  // Set synchronously, before the first await, so a second click cannot get
  // past the guard above while this one waits on the permission prompt.
  browserStarting = true;

  // This attempt's streams stay local until it becomes the live capture. They
  // used to go straight into the shared _streams, where a second attempt's
  // reset orphaned the first one's tracks with the microphone still open.
  const streams = [];
  const drop = () => streams.forEach(s => s.getTracks().forEach(t => t.stop()));
  let actx = null;
  // Only the very last statement sets this. Everything before it is an attempt
  // that has to leave nothing behind, whichever way it ends.
  let live = false;
  try {
    try {
      if (kind === 'mic' || kind === 'both') {
        streams.push(await navigator.mediaDevices.getUserMedia(
          {audio: {echoCancellation: false, noiseSuppression: false}}));
      }
      if (kind === 'system' || kind === 'both') {
        const ds = await navigator.mediaDevices.getDisplayMedia({video: true, audio: true});
        ds.getVideoTracks().forEach(t => t.stop());  // we only want the audio
        streams.push(ds);
      }
    } catch (e) { toast(t('toast.noPermission')); return; }

    // Mix every captured stream into one track (mic + system together).
    actx = new (window.AudioContext || window.webkitAudioContext)();
    const dest = actx.createMediaStreamDestination();
    let hasAudio = false;
    streams.forEach(s => {
      if (s.getAudioTracks().length) {
        actx.createMediaStreamSource(s).connect(dest); hasAudio = true;
      }
    });
    if (!hasAudio) { toast(t('toast.noAudio')); return; }

    // Past every await and every way out: this attempt is the capture now, so
    // publish its resources into the globals the rest of the page works with.
    _streams = streams; _actx = actx;
    _analyser = _actx.createAnalyser(); _analyser.fftSize = 512;
    _abuf = new Uint8Array(_analyser.fftSize);
    _actx.createMediaStreamSource(dest.stream).connect(_analyser);

    _chunks = [];
    const mime = _pickMime();
    _mr = new MediaRecorder(dest.stream, mime ? {mimeType: mime} : undefined);
    _mr.ondataavailable = e => { if (e.data && e.data.size) _chunks.push(e.data); };
    _finalized = false;
    // Every callback below names the take it was set up for, so nothing this
    // recorder says can be acted on once a later one holds the microphone.
    const take = ++_takeId;
    _mr.onstop = () => onBrowserStop(take);
    // An encoder that gives up used to be silent: no onstop, browserRec stuck
    // true, streams still open, and the only way out a page reload.
    //
    // Report it, but do NOT finish the take here. The recording spec ends an
    // errored recording the ordinary way — a final `dataavailable`, then `stop`
    // — so finalizing on the error as well uploaded the chunks captured so far
    // as one take and the final chunk as a second, competing one. Let onstop do
    // the handoff; the timer is only for a browser that sends no stop event.
    _mr.onerror = () => {
      if (_finalized || take !== _takeId) return;
      toast(t('toast.recError'));
      clearTimeout(_stopTimer);
      _stopTimer = setTimeout(() => onBrowserStop(take), _stopFallbackMs);
    };

    // The recorder is fed by the AudioContext destination, whose track never
    // ends — so the browser's own "Stop sharing" bar was invisible here and the
    // page went on cheerfully recording silence. Watch the real sources.
    streams.forEach(s => s.getAudioTracks().forEach(track => {
      track.addEventListener('ended', () => {
        if (!browserRec) return;
        toast(t('toast.sourceEnded'));
        stopBrowserCapture();   // keeps the take; onstop uploads it
      });
    }));

    _mr.start(1000);
    browserRec = true; browserPaused = false; _cancelled = false;
    _startTs = performance.now(); _pausedMs = 0; _pauseTs = 0;
    live = true;
  } catch (e) {
    // Everything after the permission prompt — the AudioContext, the audio
    // graph, the MediaRecorder constructor, start() — used to fall straight
    // through the finally below, which only lowered browserStarting. The
    // microphone stayed open, invisibly, until the page was reloaded.
    toast(t('toast.captureFailed'));
  } finally {
    if (!live) {
      drop();
      if (actx) { try { actx.close(); } catch (e) {} }
      // The globals may already point at this attempt's resources; drop() and
      // close() above have dealt with the objects, so just let go of them.
      _mr = null; _chunks = [];
      _streams = []; _actx = null; _analyser = null; _abuf = null;
      browserRec = false;
    }
    browserStarting = false;
  }
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
// A take that has been captured but not yet accepted by the server. From the
// moment the capture is torn down this Blob is the only copy in existence, so
// it is held here — with something on screen to press — until the server has
// it or the user says to let it go. It used to live in a local variable that
// went out of scope the instant the upload failed: a dropped connection, a
// busy server or a 403 turned an hour of meeting into a two-second toast.
// _pendingId identifies which take currently occupies the slot. An upload only
// clears the slot if it is still the one it was given, because "the upload
// finished" and "the take on screen is that upload's" are different questions:
// a retry that completed after a newer take had replaced it used to clear the
// newer one, throwing away a recording the server had never seen.
let _pendingBlob = null, _pendingName = '', _pendingId = 0;

function showPending(message) {
  $('pendingMsg').textContent = message || '';
  $('pendingRow').style.display = 'flex';
}
function clearPending() {
  _pendingBlob = null; _pendingName = '';
  _pendingId += 1;            // whatever held the slot no longer does
  $('pendingRow').style.display = 'none';
}

// Hands the take to the pipeline. Returns whether the server took it; keeps the
// Blob and raises the retry row when it did not. Busy for its whole duration —
// including from Retry, which used to leave the transport open and let a second
// recording start on top of an upload in flight.
async function uploadRecording(blob, name) {
  _pendingBlob = blob; _pendingName = name;
  const mine = ++_pendingId;
  browserUploading = true;
  let message = '';
  try {
    const fd = new FormData(); fd.append('file', blob, name);
    const r = await (await fetch('/api/process_file',
      {method: 'POST', headers: authHeaders(), body: fd})).json();
    if (!r.error) {
      // Only clear if this upload still owns the slot.
      if (_pendingId === mine) clearPending();
      return true;
    }
    message = r.error;
  } catch (e) { message = t('toast.uploadErr'); }
  finally { browserUploading = false; }
  toast(message);
  if (_pendingId === mine) showPending(message);
  return false;
}

$('pendingRetry').onclick = async () => {
  if (!_pendingBlob || browserUploading) return;
  $('pendingRetry').disabled = true;
  toast(t('toast.processing'));
  try { await uploadRecording(_pendingBlob, _pendingName); }
  finally { $('pendingRetry').disabled = false; poll(); }
};
// The way out that does not depend on the server working at all.
$('pendingSave').onclick = () => {
  if (!_pendingBlob) return;
  const url = URL.createObjectURL(_pendingBlob);
  const a = document.createElement('a');
  a.href = url; a.download = _pendingName;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
};
$('pendingDrop').onclick = async () => {
  if (!_pendingBlob) return;
  const go = await ask({
    title: t('ask.discardTitle'),
    body:  t('pending.dropAsk'),
    yes:   t('ask.discard'),
  });
  // The take could have been resolved while the question was on screen.
  if (!go || !_pendingBlob) return;
  clearPending();
  toast(t('toast.pendingDropped'));
};

async function onBrowserStop(take) {
  // Only the take that is still live may be finished. A caller with no take —
  // the recovery paths, and the tests — means "whichever one that is".
  if (take !== undefined && take !== _takeId) return;
  // Idempotent: a take is handed off exactly once. Both the stop event and the
  // error fallback lead here, and running twice split one recording across two
  // uploads — the second carrying only whatever arrived after the first ran.
  if (_finalized) return;
  _finalized = true;
  // Nothing is waiting for a stop event any more. Left scheduled, this is the
  // timer that used to come back two seconds later and end the next recording.
  clearTimeout(_stopTimer); _stopTimer = null;

  browserRec = false; browserPaused = false;
  const type = (_chunks[0] && _chunks[0].type) || 'audio/webm';
  const blob = new Blob(_chunks, {type}); _chunks = [];
  _cleanupStreams();
  if (_cancelled) { _cancelled = false; toast(t('toast.recCancelled')); poll(); return; }
  if (!blob.size) { toast(t('toast.emptyRec')); poll(); return; }
  const ext = type.includes('ogg') ? 'ogg' : 'webm';
  // Busy across the handoff: browserRec is already false, so without this the
  // next poll would put Record back within 700ms and a second take could start
  // on top of the multipart upload still in flight.
  browserUploading = true;
  toast(t('toast.processing'));
  try {
    await uploadRecording(blob, 'recording.' + ext);
  } finally {
    browserUploading = false;
  }
  poll();
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
  // Truncated in the middle of a path, so the whole of it has to be reachable.
  $('libDir').title = d.output_dir || '';
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

// `tx` / `sum` name which saved transcript and summary to show. A different
// recording always starts at its originals; the server falls back to them
// anyway if a name is asked for that is no longer on disk, and answers with
// what it actually opened.
async function openLibraryItem(id, tx, sum) {
  if (libCurrent !== id) { tx = ''; sum = ''; }
  const q = '/api/library/item?id=' + encodeURIComponent(id) +
            '&transcript=' + encodeURIComponent(tx || '') +
            '&summary=' + encodeURIComponent(sum || '');
  let d;
  try { d = await api(q); }
  catch (e) { toast(t('lib.loadErr')); return; }
  if (d.error) { toast(d.error); return; }
  // A failure belongs to the session it was asked for; carrying it onto the
  // next one someone opens would read as a fault of that recording.
  if (libCurrent !== id) { $('libAudio').pause(); closeLibRun(); libRunError = ''; }
  libCurrent = id; libItem = d;
  libTx  = d.transcript_name || '';
  libSum = d.summary_name || '';
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
  // The element truncates, so the whole path has to be reachable somehow.
  $('libPath').title = d.path || '';

  // The version pickers appear only once there is a choice: one transcript and
  // one summary is the normal state, and an empty dropdown beside each panel
  // would just be furniture.
  fillVariantPick('libTxPick', d.transcripts, libTx);
  fillVariantPick('libSumPick', d.summaries, libSum);
  syncLibRunBtns();

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
  // The transport draws itself from the element, so it has to be told when the
  // element changed underneath it — a new recording starts at 0:00, and the
  // one whose audio is gone must not keep the old length on screen.
  plPaint();

  // transcript.json keeps the speakers and timestamps, so it renders exactly
  // like the live panel; the .txt is the fallback for older sessions.
  const tx = $('libTranscript');
  // Every branch below replaces the panel's contents, and only the first one
  // reaches renderTranscript. Without this, opening a session whose transcript
  // is a plain .txt would leave the previous recording's lines in the index,
  // pointing at elements that are no longer on the page.
  plIndexReset();
  if (d.transcript && d.transcript.lines && d.transcript.lines.length) {
    // Seekable only when this recording actually kept its audio — a session
    // saved with Save Audio off has a transcript and nothing to play.
    renderTranscript(d.transcript, tx, !!d.audio);
  } else if (d.transcript_text) {
    tx.innerHTML = '';
    const raw = document.createElement('div');
    raw.className = 'lib-raw';
    raw.textContent = d.transcript_text;
    tx.appendChild(raw);
  } else {
    tx.innerHTML = '<span class="empty">' + esc(t('lib.noTx')) + '</span>';
  }

  // plPaint() ran above, before the lines existed. Light the right one now that
  // they do, so opening a recording part-way through lands on the moment it is
  // already at rather than waiting for the next tick of the clock.
  plHighlight();

  renderSummary(d.summary, $('libSummary'), t('lib.noSum'));
}

// ===== library transport =====
// Everything the native bar did, drawn in the app's own palette. The <audio>
// element still does the work; these handlers only move numbers onto it and
// read them back out.
const PL_RATES = [1, 1.25, 1.5, 2];

// h:mm:ss past an hour, m:ss below it — a two-hour meeting and a two-minute
// note should both read naturally.
function plClock(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const s = Math.floor(sec % 60), m = Math.floor(sec / 60) % 60;
  const h = Math.floor(sec / 3600);
  const mm = h ? String(m).padStart(2, '0') : String(m);
  return (h ? h + ':' : '') + mm + ':' + String(s).padStart(2, '0');
}

// How many ticks the ladder is drawn with. Fixed rather than derived from the
// width: they are spread by the flexbox, so the count only sets how fine the
// scale reads, and a fixed one means no relayout work on resize.
const PL_BARS = 80;
let plLit = -1;   // how many are currently lit, so a repaint touches the delta

// Lights the first `n` ticks. Called on every timeupdate, which is a few times
// a second, so it only writes to the ones that actually changed.
function plPaintBars(fraction) {
  const bars = $('plBars').children;
  if (!bars || !bars.length) return;
  const lit = Math.max(0, Math.min(bars.length, Math.round(fraction * bars.length)));
  if (lit === plLit) return;
  const from = plLit < 0 ? 0 : Math.min(lit, plLit);
  const to   = plLit < 0 ? bars.length : Math.max(lit, plLit);
  for (let i = from; i < to; i++) bars[i].classList.toggle('on', i < lit);
  plLit = lit;
}

// ---- following the transcript while it plays ----
// The line being spoken is lit as the recording runs, so a saved session can be
// read and listened to at the same time. Built from the same start/end the
// clickable timestamps use, so the highlight and a click on a stamp always
// agree about which line a moment belongs to.
let plLines = [];      // [{start, end, el}], in order, library panel only
let plLineLit = null;      // the element currently lit, so it can be un-lit
let plScrolledAt = 0;  // when the reader last scrolled by hand

// Following is one feature — light the line, keep it on screen — and one
// switch turns both halves off, because half of it is what makes the other
// half worth having. On unless the reader has said otherwise, and remembered
// across reloads: someone who reads at their own pace says so once.
let plFollowOn = true;
const PL_FOLLOW_KEY = 'transcriptor-follow';

// Seconds past a line's end before the highlight goes dark. Turns usually butt
// up against each other, and blinking the highlight off in the fraction of a
// second between them would be worse than holding it; a real silence is longer
// than this and does clear it.
const PL_GAP_GRACE = 1.5;

function plIndexReset() {
  plLines = [];
  plLineLit = null;
}

// The last line that has started by `sec`, or null in a silence.
function plLineAt(sec) {
  let lo = 0, hi = plLines.length - 1, found = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (plLines[mid].start <= sec) { found = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  if (found < 0) return null;
  return sec <= plLines[found].end + PL_GAP_GRACE ? plLines[found] : null;
}

// Bring the lit line back into view, but never fight someone who is reading
// ahead: a scroll of their own buys a few seconds of being left alone.
// `force` is the reader asking to be caught up — switching following back on —
// which outranks the grace their last scroll bought them.
function plFollow(el, force) {
  if (!force && Date.now() - plScrolledAt < 4000) return;
  const box = $('libTranscript');
  if (!box || !box.getBoundingClientRect || !el.getBoundingClientRect) return;
  const b = box.getBoundingClientRect(), r = el.getBoundingClientRect();
  if (!b.height) return;
  if (r.top >= b.top && r.bottom <= b.bottom) return;   // already on screen
  box.scrollTop += (r.top - b.top) - (b.height - r.height) / 2;
}

function plHighlight() {
  const a = $('libAudio');
  const line = (plFollowOn && plLines.length && a.getAttribute('src'))
    ? plLineAt(a.currentTime || 0) : null;
  const next = line ? line.el : null;
  if (next === plLineLit) return;
  if (plLineLit && plLineLit.classList) plLineLit.classList.remove('at');
  plLineLit = next;
  if (!plLineLit || !plLineLit.classList) return;
  plLineLit.classList.add('at');
  // Only chase the line while it is actually playing. Scrubbing or seeking
  // moves the highlight too, and yanking the panel around under a stationary
  // cursor is not helpful.
  if (!a.paused) plFollow(plLineLit);
}

// The one place the switch is thrown. `remember` is false for the reading at
// startup, which is only painting what was already decided.
function plSetFollow(on, remember) {
  plFollowOn = !!on;
  const b = $('plFollowBtn');
  if (b) {
    b.classList.toggle('off', !plFollowOn);
    b.setAttribute('aria-pressed', plFollowOn ? 'true' : 'false');
  }
  if (remember) {
    try { localStorage.setItem(PL_FOLLOW_KEY, plFollowOn ? '1' : '0'); } catch (e) {}
  }
  // Switching off clears the lit line; switching on lands on the moment the
  // recording is at, paused or not, rather than waiting for the next line.
  plScrolledAt = 0;
  plHighlight();
  if (plFollowOn && plLineLit) plFollow(plLineLit, true);
}

function plPaint() {
  const a = $('libAudio');
  const dur = a.duration, cur = a.currentTime || 0;
  const known = isFinite(dur) && dur > 0;
  const p = known ? Math.min(1, Math.max(0, cur / dur)) : 0;
  const pct = p * 100;

  plPaintBars(p);
  $('plHead').style.left  = pct + '%';
  $('plNow').textContent  = plClock(cur);
  // "--:--" rather than 0:00 while the length is still unknown: a zero there
  // reads as an empty file.
  $('plDur').textContent  = known ? plClock(dur) : '--:--';

  const track = $('plTrack');
  track.setAttribute('aria-valuemax', known ? Math.floor(dur) : 0);
  track.setAttribute('aria-valuenow', Math.floor(cur));
  track.setAttribute('aria-valuetext', plClock(cur));

  plHighlight();
}

function plSeekAt(clientX) {
  const a = $('libAudio'), track = $('plTrack');
  if (!isFinite(a.duration) || a.duration <= 0 || !track.getBoundingClientRect) return;
  const r = track.getBoundingClientRect();
  const p = Math.min(1, Math.max(0, (clientX - r.left) / (r.width || 1)));
  a.currentTime = p * a.duration;
  plPaint();
}

function plToggle() {
  const a = $('libAudio');
  if (!a.getAttribute('src')) return;
  // play() rejects when the src is gone or the format is refused; the button
  // state is driven by the events below either way, so nothing to do here.
  if (a.paused) { const p = a.play(); if (p && p.catch) p.catch(() => {}); }
  else a.pause();
}

(function setupPlayer() {
  const a = $('libAudio'), track = $('plTrack');

  const bars = $('plBars');
  for (let i = 0; i < PL_BARS; i++) bars.appendChild(document.createElement('i'));

  ['timeupdate', 'durationchange', 'loadedmetadata', 'seeked', 'emptied']
    .forEach(ev => a.addEventListener(ev, plPaint));
  a.addEventListener('play',  () => $('plPlay').classList.add('on'));
  a.addEventListener('pause', () => $('plPlay').classList.remove('on'));
  a.addEventListener('ended', () => $('plPlay').classList.remove('on'));

  $('plPlay').addEventListener('click', plToggle);

  // Dragging keeps seeking after the pointer leaves the track, which is how
  // every other scrubber behaves and how a two-hour recording stays usable.
  let dragging = false;
  track.addEventListener('pointerdown', e => {
    dragging = true;
    if (track.setPointerCapture) track.setPointerCapture(e.pointerId);
    plSeekAt(e.clientX);
  });
  track.addEventListener('pointermove', e => { if (dragging) plSeekAt(e.clientX); });
  const drop = e => {
    dragging = false;
    if (track.releasePointerCapture && e.pointerId !== undefined) {
      try { track.releasePointerCapture(e.pointerId); } catch (_) {}
    }
  };
  track.addEventListener('pointerup', drop);
  track.addEventListener('pointercancel', drop);

  track.addEventListener('keydown', e => {
    const step = e.shiftKey ? 30 : 5;      // Shift for a coarser jump
    const cur = a.currentTime || 0;
    if (e.key === 'ArrowRight')     a.currentTime = isFinite(a.duration) ? Math.min(a.duration, cur + step) : cur + step;
    else if (e.key === 'ArrowLeft') a.currentTime = Math.max(0, cur - step);
    else if (e.key === 'Home')      a.currentTime = 0;
    else if (e.key === 'End' && isFinite(a.duration)) a.currentTime = a.duration;
    else if (e.key === ' ' || e.key === 'Enter') plToggle();
    else return;
    e.preventDefault();
    plPaint();
  });

  $('plMute').addEventListener('click', () => {
    a.muted = !a.muted;
    $('plMute').classList.toggle('off', a.muted);
  });

  $('plRate').addEventListener('click', () => {
    const next = PL_RATES[(PL_RATES.indexOf(a.playbackRate) + 1) % PL_RATES.length];
    a.playbackRate = next;
    $('plRate').textContent = next + '×';
    $('plRate').classList.toggle('on', next !== 1);
  });

  // Anything but a stored "off" is on, so a first visit and a wiped store both
  // start following.
  let followPref = null;
  try { followPref = localStorage.getItem(PL_FOLLOW_KEY); } catch (e) {}
  plSetFollow(followPref !== '0', false);
  $('plFollowBtn').addEventListener('click', () => plSetFollow(!plFollowOn, true));
})();

// ---- saved versions, and running the models again ----
// A re-run can replace what a session already has or sit beside it under a
// name. Both are on disk as files — transcript.<name>.json, summary.<name>.txt
// — so the older ones stay openable from the pickers rather than being
// something the new run consumed.
let libTx = '', libSum = '';   // which saved transcript / summary is on screen
let libRunKind = '';           // 'transcribe' | 'summarize' while the form is open
let libRunTimer = null;
let libJobBusy = false;        // the one worker, as of the last poll
// Why a re-run started here stopped, until another is asked for. Held rather
// than read off the phase so that a studio job that failed does not put a red
// box over whatever recording the library happens to have open.
let libRunError = '';

// Re-transcribing needs the audio and re-summarizing needs a transcript;
// neither can start while a job holds the only worker. Both halves of that are
// known in different places, so both call this rather than each disabling the
// buttons on what it alone knows — which is how a poll came to re-enable a
// button for a session that has no audio to run.
function syncLibRunBtns() {
  const d = libItem;
  $('libRetx').disabled  = libJobBusy || !(d && d.audio);
  $('libResum').disabled = libJobBusy || !(d && d.transcripts && d.transcripts.length);
}

// The wrapper is what is on screen once enhanceSelect() has run; hiding the
// native element it replaced would leave the dropdown sitting there.
function showSelect(id, on) {
  const s = $(id);
  if (!s) return;
  const el = (s._x && s._x.wrap) ? s._x.wrap : s;
  el.hidden = !on;
}

function fillVariantPick(id, variants, current) {
  const sel = $(id);
  const list = variants || [];
  sel.innerHTML = '';
  list.forEach(v => {
    const o = document.createElement('option');
    o.value = v.name;
    o.textContent = v.name || t('lib.original');
    sel.appendChild(o);
  });
  sel.value = current || '';
  refreshSelect(id);
  showSelect(id, list.length > 1);
}

$('libTxPick').addEventListener('change', () => {
  if (libCurrent) openLibraryItem(libCurrent, $('libTxPick').value, libSum);
});
$('libSumPick').addEventListener('change', () => {
  if (libCurrent) openLibraryItem(libCurrent, libTx, $('libSumPick').value);
});

// The library's readout of the job, from the same poll and the same state the
// studio reports. There is one job slot, so progress is shown for whichever
// run holds it — a re-run started here, or a take still being processed on the
// other tab, which is also why the buttons are dim. Once the worker is free
// the strip clears, except for a failure this tab asked for: that stays until
// the next run, because a toast is gone before it explains anything.
function renderLibStatus(s) {
  const box = $('libStatus');
  if (!box) return;
  const failed = !s.processing && !!libRunError;
  box.hidden = !(s.processing || failed);
  libJobBusy = !!s.processing;
  syncLibRunBtns();
  if (box.hidden) return;

  box.classList.toggle('err', failed);
  $('libStop').hidden = !s.processing;
  $('libStatusMsg').textContent = failed ? libRunError : (s.message || '');
  renderProgress('libStatusPct', 'libStatusTrack', 'libStatusFill', jobFraction(s));
}

function closeLibRun() {
  libRunKind = '';
  $('libRun').hidden = true;
  $('libRunName').value = '';
}

// Opens the form, or starts straight away when there is nothing to overwrite:
// asking "replace or keep both?" about a session with no summary at all is a
// question with one real answer.
function openLibRun(kind) {
  if (!libItem) return;
  const existing = kind === 'transcribe' ? (libItem.transcripts || [])
                                         : (libItem.summaries || []);
  if (!existing.length) { startLibRun(kind, ''); return; }

  libRunKind = kind;
  $('libRunQ').textContent =
    t(kind === 'transcribe' ? 'lib.runAskTx' : 'lib.runAskSum');
  $('libRunOver').textContent =
    t(kind === 'transcribe' ? 'lib.runOverTx' : 'lib.runOverSum',
      {name: (kind === 'transcribe' ? libTx : libSum) || t('lib.original')});
  $('libRun').querySelector('input[value="overwrite"]').checked = true;
  $('libRunName').value = '';
  $('libRun').hidden = false;
}

async function startLibRun(kind, name) {
  const id = libCurrent;
  if (!id) return;
  const body = {id: id, name: name};
  if (kind === 'summarize') {
    // Summarize the transcript that is on screen, with the template and the
    // context the studio panel is set to — the same request the live button
    // would make, aimed at a recording that is already saved.
    body.source = libTx;
    body.template = $('tpl').value;
    body.title = $('ctxTitle').value;
    body.participants = $('ctxPeople').value;
    body.notes = $('ctxNotes').value;
  }
  const r = await post('/api/library/' + kind, body);
  if (r.error) { libRunError = r.error; toast(r.error); poll(); return; }
  libRunError = '';
  closeLibRun();
  toast(t(kind === 'transcribe' ? 'lib.runningTx' : 'lib.runningSum'));
  // The readout above carries the progress; this only needs to know when it is
  // over, so the panel can show what landed.
  watchLibRun(kind, name);
  poll();   // put the readout up now rather than at the next tick
}

// One poll, ending when the job does. The result is a file, so the item is
// simply re-opened — with the new version selected, which is what someone who
// just asked for it wants to look at.
function watchLibRun(kind, name) {
  if (libRunTimer) clearInterval(libRunTimer);
  libRunTimer = setInterval(async () => {
    let s;
    try { s = await api('/api/state'); } catch (e) { return; }
    if (s.processing) return;
    clearInterval(libRunTimer); libRunTimer = null;
    await libRunEnded(kind, name, s);
  }, 900);
}

// A stopped run ends on "idle", the same phase a finished one reaches once the
// studio moves on, so job_cancelled is what tells them apart. Reading only
// the error phase announced a stopped run as done and went to open a version
// it never wrote.
async function libRunEnded(kind, name, s) {
  const stopped = !!s.job_cancelled && s.phase !== 'done' && s.phase !== 'error';
  // Recorded before the panel is rebuilt: the readout above outlives the
  // toast, which is the whole point of it for a run that failed.
  libRunError = s.phase === 'error' ? (s.message || t('lib.runFailed')) : '';
  if (libRunError) toast(libRunError);
  else toast(t(stopped ? 'lib.runStopped' : 'lib.runDone'));
  if (!libCurrent) return;
  await loadLibrary();
  // Whatever was on screen is still there after a stop; stay on it.
  const landed = !libRunError && !stopped;
  openLibraryItem(libCurrent, landed && kind === 'transcribe' ? name : libTx,
                  landed && kind === 'summarize' ? name : libSum);
}

$('libStop').onclick = async () => {
  const q = CANCEL_ASK.job;
  const go = await ask({title: t(q.title), body: t(q.body), yes: t(q.yes), no: t(q.no)});
  if (!go) return;
  const r = await post('/api/cancel', {job_only: true});
  if (r && r.error) { toast(r.error); return; }
  toast(t(r && r.stopped === 'job' ? 'toast.jobStopped' : 'toast.jobAlreadyDone'));
  poll();
};

$('libRetx').onclick  = () => openLibRun('transcribe');
$('libResum').onclick = () => openLibRun('summarize');
$('libRunCancel').onclick = closeLibRun;
$('libRunStart').onclick = () => {
  const mode = $('libRun').querySelector('input[name="libRunMode"]:checked');
  const wantNew = mode && mode.value === 'new';
  // Overwrite means the version the dialog just named — the one on screen —
  // which is only the session's original when that is what is selected. An
  // empty name is how the server is told "the original", so sending it
  // unconditionally aimed every replacement at transcript.txt / summary.txt:
  // the file the user had not asked about was destroyed, and the named version
  // they were looking at came back unchanged.
  const shown = libRunKind === 'transcribe' ? libTx : libSum;
  const name = wantNew ? $('libRunName').value.trim() : shown;
  if (wantNew && !name) { toast(t('lib.runNameNeeded')); return; }
  // Dots and separators would let a name reach past its own file, and Windows
  // refuses the rest in a file name; the server refuses them too, but saying
  // so here costs nothing and comes before a run, not after it.
  if (wantNew && /[./\\:<>"|?*]/.test(name)) { toast(t('lib.runNameBad')); return; }
  startLibRun(libRunKind, name);
};

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
  const id = libCurrent;
  const s = libSessions.find(x => x.id === id);
  const name = s ? libDate(s) : id;
  const go = await ask({
    title: t('ask.deleteTitle'),
    body:  t('lib.deleteAsk', {name: name, path: libItem.path || id}),
    yes:   t('lib.delete'),
  });
  // Asking is no longer instant, so the selection may have moved under it —
  // and deleting whatever happens to be selected now is not what was agreed to.
  if (!go || libCurrent !== id) return;

  const r = await post('/api/library/delete', {id: id});
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
