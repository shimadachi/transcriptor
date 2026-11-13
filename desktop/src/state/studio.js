// The studio: the poll that keeps the window in step with the engine, and what
// the transport's buttons do.
import {api, post, upload} from '../lib/api.js';
import {transcriptHasText} from '../lib/format.js';
import {app} from '../lib/store.js';
import {BROWSER_SOURCES, CANCEL_ASK} from '../lib/tables.js';
import {t} from '../i18n.js';
import {cancelCapture, captureElapsed, captureLevel, captureState, onCaptureChange,
        startCapture, stopCapture, toggleCapturePause} from './capture.js';
import {loadLibrary, onLibraryChange} from './library.js';
import {onLangChange} from './prefs.js';
import {showWindow, syncTray} from './tray.js';
import {ask, toast} from './ui.js';

export const POLL_MS = 700;

// ---- what the transport allows ----
// One reading of the rules, used by the buttons to draw themselves and by the
// tray to decide whether a press may go through: a disabled button stays
// unpressed whoever is pressing it.
export function transport(st) {
  const s = st.s || {};
  const cap = st.capture;
  const rec = st.view.rec;
  const busy = rec || !!s.processing || cap.starting || cap.uploading;
  return {
    busy,
    // Only the models can't be interrupted -- and a capture that is still
    // asking for permission, which has nothing to stop yet, or one whose take
    // is still on its way to the engine.
    recDisabled: st.recLatch || !!s.processing || cap.starting || cap.uploading,
    pauseDisabled: !rec,
    // Cancel: while a job runs it stops the models; otherwise it discards the
    // take or result on screen.
    cancelDisabled: cap.uploading ||
      !(s.processing || rec || s.has_audio || s.has_result || s.has_summary ||
        s.phase === 'error'),
    // Transcribe stays live after a run -- the same audio can go through again
    // with another model, or with speaker separation switched on -- but not
    // without the weights on disk: pressing it used to start a job whose first
    // minutes were a silent 3 GB download.
    txDisabled: busy || !s.has_audio || !s.stt_cached,
    // Nothing to summarize without words, whatever has_result says.
    sumNoWords: !s.has_result || !st.txHasText,
    sumDisabled: busy || !s.has_result || !st.txHasText,
  };
}

// ---- sources ----
let sourcesGen = 0;
export async function loadSources() {
  // Fetch first, touch the state only afterwards: start-up and a language
  // change both load sources. The generation guard drops a response that a
  // newer load has already overtaken.
  const gen = ++sourcesGen;
  let d;
  try { d = await api('/api/sources'); } catch { d = {sources: []}; }
  if (gen !== sourcesGen) return;
  const sources = (d && d.sources) || [];

  // Kept across a reload so refreshing (or switching language) does not throw
  // away the source the user picked. Otherwise a real engine-side source is
  // preferred; with none, recording in this window is the way.
  const {source: prevSrc, mic: prevMic} = app.get();
  const ids = BROWSER_SOURCES.map(b => b.id).concat(sources.map(s => s.id));
  let source = BROWSER_SOURCES[0].id;
  if (sources.some(s => s.id === d.default_id)) source = d.default_id;
  if (prevSrc && ids.includes(prevSrc)) source = prevSrc;
  const mics = sources.filter(s => !s.is_loopback);
  const mic = prevMic && mics.some(m => m.id === prevMic) ? prevMic : '';
  app.set({sources, source, mic});
}

// The note-template menu, from settings (labels + the saved default).
export async function loadTemplates() {
  try {
    const s = await api('/api/settings');
    const templates = s.templates || [];
    const saved = templates.some(x => x.value === s.summary_template);
    app.set({templates,
             tpl: saved ? s.summary_template : (templates[0] ? templates[0].value : '')});
  } catch { /* the engine is not ready yet */ }
}

// Persist the template choice when changed from the summary panel.
export function pickTemplate(value) {
  app.set({tpl: value});
  post('/api/settings', {summary_template: value});
}

// ---- results ----
let prevResultRev = 0, prevSummaryRev = 0;
// One fetch at a time: each poll tick is 700ms and a fetch can outlast that,
// so several can be in the air at once.
//
// Discarding the superseded responses instead -- a generation counter --
// starved the panels outright whenever responses were consistently slower than
// the interval: every response was already stale by the time it arrived,
// nothing rendered, no revision was ever committed, and each tick launched yet
// another request. Declining to start a second one has the same protection
// against out-of-order renders and cannot starve: the caller leaves the
// revision uncommitted, so the next tick simply tries again.
let resultLoading = false;

// Returns whether this call put a result on screen.
async function loadResult() {
  if (resultLoading) return false;
  resultLoading = true;
  try {
    const d = await api('/api/result');
    // The panel holds the result now, whatever it held before; a preview that
    // is still wanted is drawn again from scratch on this same tick.
    leaveLive();
    app.set({
      result: d.result || null,
      txHasText: transcriptHasText(d.result),
      txEmptyKey: 'tx.none',
      summary: d.summary || null,
      summaryCut: !!(d.summary && d.summary_cut_short),
    });
    return true;
  } finally {
    resultLoading = false;
  }
}

// ---- live transcript ----
// The preview the studio's switch puts in the transcript panel while there is
// no finished transcript to show there instead. Within a take the preview's
// settled lines only ever grow, so the page keeps what it has and asks only
// for what came after; the engine's `take` changing is the signal to start
// again.
let liveBusy = false;       // a press of the switch is on its way to the engine
// Bumped whenever something else takes the panel. Polls overlap, so a preview
// fetched before a result landed must not be drawn over that result once it
// arrives.
let txOwner = 0;
let liveTake = -1, liveCount = 0, liveRev = -1, liveLoading = false;

// The panel is someone else's now: a result, a cleared take, the empty state.
function leaveLive() {
  txOwner++; liveTake = -1;
  if (app.get().live) app.set({live: null});
}

function drawLive(d, append) {
  const prev = append ? app.get().live.lines : [];
  liveCount = typeof d.count === 'number' ? d.count : (append ? liveCount : 0);
  app.set({live: {
    lines: prev.concat(d.lines || []),
    partial: d.partial ? {ts: d.partial_ts, text: d.partial} : null,
  }});
}

async function syncLive(s, rec) {
  const lv = s.live || {};
  // The finished transcript always wins the panel. Short of one, the preview
  // has it whenever it has something to say: a take being heard, the switch
  // on during a take -- if only to say why nothing is coming -- or the text
  // the take that just ended left behind.
  const wanted = !s.has_result && !!((rec && lv.on) || lv.active || lv.has_text);
  if (!wanted) {
    if (app.get().live) {
      leaveLive();
      app.set({result: null, txEmptyKey: s.has_audio ? 'tx.waiting' : 'tx.empty'});
    }
    return;
  }

  const same = !!app.get().live && lv.take === liveTake;
  if ((!same || lv.rev !== liveRev) && !liveLoading) {
    liveLoading = true;
    const owner = txOwner;
    try {
      const from = same ? liveCount : 0;
      const d = await api('/api/live?from=' + from);
      if (owner !== txOwner) return;   // a result took the panel meanwhile
      let drawn = true;
      if (same && d.take === liveTake && d.from === from) {
        drawLive(d, true);
      } else if (d.from === 0) {
        drawLive(d, false);
      } else {
        // Lines from part-way into a take this page has not seen are no use
        // on their own; the next tick asks again from the start.
        liveCount = 0;
        app.set({live: {lines: [], partial: null}});
        drawn = false;
      }
      liveTake = d.take;
      liveRev = drawn ? d.rev : -1;
    } catch {
      /* the next tick asks again */
    } finally {
      liveLoading = false;
    }
  }
}

export async function toggleLive() {
  if (liveBusy) return;
  liveBusy = true;
  const st = app.get();
  const now = st.liveOverride || (st.s && st.s.live) || {};
  const want = !now.on;
  // Drawn now: the press is its own answer, and the next poll is up to 700ms
  // away. Put back if the engine says no.
  app.set({liveOverride: {on: want}});
  try {
    const r = await post('/api/live', {on: want});
    if (r.error) { toast(r.error); app.set({liveOverride: {on: !want}}); }
  } catch {
    app.set({liveOverride: {on: !want}});
  } finally {
    liveBusy = false;
  }
}

// ---- the poll ----
export async function poll() {
  syncTray();
  let s;
  try { s = await api('/api/state'); } catch { return; }

  // Merge the engine's recording with one made in this window.
  const cap = captureState();
  const rec = cap.rec || !!s.recording;
  const view = {
    rec,
    paused: cap.rec ? cap.paused : !!s.paused,
    elapsed: cap.rec ? captureElapsed() : (s.elapsed || 0),
    level: cap.rec ? captureLevel() : (s.level || 0),
  };
  // The switch is left alone while a press of its own is on its way, or a
  // poll that set off before the press would put the old state back for a tick.
  const patch = {s, view, recLatch: false};
  if (!liveBusy) patch.liveOverride = null;
  app.set(patch);

  // Watch the revision, not has_result. Transcribing the same audio again never
  // lowers that flag, so a run-to-run change was invisible and the panel kept
  // showing the previous text until a summary happened to land.
  //
  // The revision moves when a result is *cleared* as well -- starting a new
  // recording or upload bumps it -- and reloading is not conditional on there
  // being something to load. Skipping the reload when has_result was false is
  // what left the last session's transcript and summary on screen beside the
  // new session's audio, indefinitely with auto-transcribe off.
  const resRev = s.result_rev || 0, sumRev = s.summary_rev || 0;
  const freshResult  = resRev !== prevResultRev;
  const freshSummary = sumRev !== prevSummaryRev;

  if (freshResult || freshSummary) {
    // Commit the revisions only once the panels have actually been rendered.
    // Committing first meant a single failed /api/result -- a blip, a restart
    // -- left the stale transcript on screen for good: every later poll then
    // compared equal and never tried the reload again.
    try {
      if (await loadResult()) { prevResultRev = resRev; prevSummaryRev = sumRev; }
    } catch { /* leave the revisions be; the next poll retries */ }
  }

  // The live preview, where there is no finished transcript to show instead.
  await syncLive(s, rec);

  // Nothing transcribed yet: say what the panel is waiting for. Runs after the
  // reload above, so a panel cleared on this same tick gets the right line
  // instead of "no transcript found".
  if (!s.has_result && !s.processing && !app.get().live) {
    const key = s.has_audio ? 'tx.waiting' : 'tx.empty';
    if (app.get().txEmptyKey !== key) app.set({txEmptyKey: key});
  }

  // A finished run just wrote a folder; refresh the list if it is on screen.
  if ((freshResult || freshSummary) && app.get().tab === 'library') loadLibrary();
}

// ---- actions ----
export async function pressRecord() {
  // One button for both halves of the transport, so a second press within the
  // 700ms poll window would otherwise read the stale state and start twice.
  // poll() releases the latch as soon as it has the real answer.
  const st = app.get();
  app.set({recLatch: true});
  // Same press stops what it started.
  if (st.capture.rec || st.view.rec) {
    if (st.capture.rec) stopCapture();
    else post('/api/record/stop');
    return;
  }
  if (st.source.startsWith('browser:')) { startCapture(st.source.split(':')[1]); return; }
  const body = {source_id: st.source};
  if (st.mic) body.mic_source_id = st.mic;   // mix this mic into the system audio
  const r = await post('/api/record/start', body);
  if (r.error) toast(r.error);
}

export async function pressPause() {
  const st = app.get();
  if (st.capture.rec) { toggleCapturePause(); poll(); return; }
  await post(st.view.paused ? '/api/record/resume' : '/api/record/pause');
}

export async function pressCancel() {
  // Which of the three this is decides the question, so read the state now
  // rather than trusting whatever the last poll left on the screen.
  let s = {};
  try { s = await api('/api/state'); } catch { /* ask anyway, below */ }
  const capturing = captureState().rec;
  const rec = capturing || s.recording;
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

  if (captureState().rec) cancelCapture();   // stop + skip upload
  // A job is asked to stop and nothing else. The check above still leaves the
  // moment between its answer and this request, and a run that finished in it
  // turned the press into a discard of the result it had just written.
  const r = await post('/api/cancel', kind === 'job' ? {job_only: true} : null);
  if (r && r.error) { toast(r.error); return; }
  // Stopping a run throws nothing away, so the panels stay as they are -- the
  // transcript a cancelled summary was reading is still the transcript.
  if (r && r.stopped === 'job') { toast(t('toast.jobStopped')); return; }
  if (r && r.stopped === 'none') { toast(t('toast.jobAlreadyDone')); poll(); return; }
  // A cancelled take or a discarded result does clear them. The revision is
  // left alone: it only ever climbs, so the next run's transcript still reads
  // as new.
  leaveLive();
  app.set({result: null, txEmptyKey: 'tx.empty', summary: null, summaryCut: false});
  toast(t('toast.cancelled'));
}

export async function processFile(file) {
  if (!file) return;
  toast(t('toast.uploading') + file.name);
  try {
    const r = await upload(file);
    if (r.error) toast(r.error);
  } catch { toast(t('toast.uploadErr')); }
}

export async function pressTranscribe() {
  const r = await post('/api/transcribe', {});
  if (r.error) toast(r.error);
}

export async function pressSummarize() {
  const st = app.get();
  const r = await post('/api/summarize', {
    template: st.tpl,
    title: st.ctx.title,
    participants: st.ctx.people,
    notes: st.ctx.notes,
  });
  if (r.error) toast(r.error);
}

export async function openOutputFolder() {
  const r = await post('/api/open_folder');
  if (r.error) toast(r.error); else if (!r.ok) toast(t('toast.folderErr') + r.path);
}

// The speaker models and the voice detector have nothing to choose between,
// so their cautions fetch them where they stand and carry the progress.
export async function fetchModel(kind) {
  const r = await post('/api/model/download', {kind, id: kind});
  if (r.error) toast(r.error);
}

// A link in a summary, or the update banner, opens in the real browser: the
// window never navigates away from the app. The engine hands the URL to the OS
// after holding it to its own rules; if it will not, opening a window is
// refused by the app and passed to the OS the same way (electron/main.js).
export function openLink(url) {
  const fallback = () => window.open(url, '_blank', 'noopener');
  post('/api/open_link', {url}).then(r => { if (!r || !r.ok) fallback(); }, fallback);
}

// ---- the tray's Start/Stop and Pause ----
// They go through the same functions as a click, so a take started from the
// tray records from the source picked here and is held to the same rules.
export async function trayAction(what) {
  // A window hidden in the tray has its timers throttled. Act on the state as
  // it is now, not as the last poll left it.
  await poll();
  const st = app.get();
  const rules = transport(st);
  if (what === 'pause' ? rules.pauseDisabled : rules.recDisabled) return;
  const startingTake = what === 'record' && !st.view.rec;
  // A capture recorded in the page has to begin in the page: a permission
  // prompt in a hidden window would wait there with nobody to answer it.
  if (startingTake && st.source.startsWith('browser:')) {
    showWindow();
    toast(t('toast.trayPressRecord'));
    return;
  }
  if (what === 'pause') await pressPause(); else await pressRecord();
  syncTray();
}

// What the recorder, the library and a language change need from here, handed over rather
// than imported back, so the modules do not depend on each other in a circle.
onCaptureChange(() => { poll(); });
onLibraryChange(() => { poll(); });
onLangChange(() => { loadSources(); loadTemplates(); });
