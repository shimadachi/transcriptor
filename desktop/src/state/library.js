// The library: past sessions read straight out of the output folder. The
// engine does the scanning; nothing is cached here beyond the current listing.
import {api, post} from '../lib/api.js';
import {libDate} from '../lib/format.js';
import {app} from '../lib/store.js';
import {CANCEL_ASK} from '../lib/tables.js';
import {t} from '../i18n.js';
import {ask, toast} from './ui.js';

const lib = () => app.get().lib;
const setLib = patch => app.set(st => ({lib: {...st.lib, ...patch}}));

// The player's <audio> element, while the detail pane is mounted. The actions
// here stop it when what it is playing goes away or off screen.
let audioEl = null;
export function registerAudio(el) { audioEl = el; }
export function pauseLibraryAudio() { if (audioEl) audioEl.pause(); }

// What studio.js's poll does, without importing it back in a circle.
let refresh = () => {};
export function onLibraryChange(fn) { refresh = fn; }

export async function loadLibrary() {
  setLib({loading: true});
  let d;
  try { d = await api('/api/library'); } catch { d = {sessions: []}; }
  const sessions = d.sessions || [];
  const patch = {sessions, dir: d.output_dir || '', loading: false};
  // A recording deleted outside the app leaves the detail pane pointing at
  // nothing; drop the selection rather than showing a stale one.
  const {current} = lib();
  if (current && !sessions.some(x => x.id === current)) {
    patch.current = null; patch.item = null;
    pauseLibraryAudio();
  }
  setLib(patch);
}

// `tx` / `sum` name which saved transcript and summary to show. A different
// recording always starts at its originals; the engine falls back to them
// anyway if a name is asked for that is no longer on disk, and answers with
// what it actually opened.
export async function openLibraryItem(id, tx, sum) {
  if (lib().current !== id) { tx = ''; sum = ''; }
  const q = '/api/library/item?id=' + encodeURIComponent(id) +
            '&transcript=' + encodeURIComponent(tx || '') +
            '&summary=' + encodeURIComponent(sum || '');
  let d;
  try { d = await api(q); }
  catch { toast(t('lib.loadErr')); return; }
  if (d.error) { toast(d.error); return; }
  const patch = {current: id, item: d,
                 tx: d.transcript_name || '', sum: d.summary_name || ''};
  // A failure belongs to the session it was asked for; carrying it onto the
  // next one someone opens would read as a fault of that recording.
  if (lib().current !== id) {
    pauseLibraryAudio();
    Object.assign(patch, {runKind: '', runError: '', runNote: ''});
  }
  setLib(patch);
}

// ---- saved versions, and running the models again ----
// A re-run can replace what a session already has or sit beside it under a
// name. Both are on disk as files -- transcript.<name>.json, summary.<name>.txt
// -- so the older ones stay openable from the pickers rather than being
// something the new run consumed.
let runTimer = null;

export function closeLibRun() { setLib({runKind: ''}); }

// Opens the form, or starts straight away when there is nothing to overwrite:
// asking "replace or keep both?" about a session with no summary at all is a
// question with one real answer.
export function openLibRun(kind) {
  const {item} = lib();
  if (!item) return;
  const existing = kind === 'transcribe' ? (item.transcripts || []) : (item.summaries || []);
  if (!existing.length) { startLibRun(kind, ''); return; }
  setLib({runKind: kind});
}

// `mode` is 'overwrite' or 'new'; `newName` is what the form's field holds.
export function submitLibRun(mode, newName) {
  const {runKind, tx, sum} = lib();
  const wantNew = mode === 'new';
  // Overwrite means the version the dialog just named -- the one on screen --
  // which is only the session's original when that is what is selected. An
  // empty name is how the engine is told "the original", so sending it
  // unconditionally aimed every replacement at transcript.txt / summary.txt:
  // the file the user had not asked about was destroyed, and the named version
  // they were looking at came back unchanged.
  const shown = runKind === 'transcribe' ? tx : sum;
  const name = wantNew ? newName.trim() : shown;
  if (wantNew && !name) { toast(t('lib.runNameNeeded')); return; }
  // Dots and separators would let a name reach past its own file, and Windows
  // refuses the rest in a file name; the engine refuses them too, but saying
  // so here costs nothing and comes before a run, not after it.
  if (wantNew && /[./\\:<>"|?*]/.test(name)) { toast(t('lib.runNameBad')); return; }
  startLibRun(runKind, name);
}

async function startLibRun(kind, name) {
  const st = app.get();
  const id = st.lib.current;
  if (!id) return;
  const body = {id, name};
  if (kind === 'summarize') {
    // Summarize the transcript that is on screen, with the template and the
    // context the studio panel is set to -- the same request the studio's
    // button would make, aimed at a recording that is already saved.
    body.source = st.lib.tx;
    body.template = st.tpl;
    body.title = st.ctx.title;
    body.participants = st.ctx.people;
    body.notes = st.ctx.notes;
  }
  const r = await post('/api/library/' + kind, body);
  if (r.error) { setLib({runError: r.error, runNote: ''}); toast(r.error); refresh(); return; }
  setLib({runError: '', runNote: '', runKind: ''});
  toast(t(kind === 'transcribe' ? 'lib.runningTx' : 'lib.runningSum'));
  // The readout carries the progress; this only needs to know when it is over,
  // so the panel can show what landed.
  watchLibRun(kind, name);
  refresh();   // put the readout up now rather than at the next tick
}

// One poll, ending when the job does. The result is a file, so the item is
// simply re-opened -- with the new version selected, which is what someone who
// just asked for it wants to look at.
function watchLibRun(kind, name) {
  if (runTimer) clearInterval(runTimer);
  runTimer = setInterval(async () => {
    let s;
    try { s = await api('/api/state'); } catch { return; }
    if (s.processing) return;
    clearInterval(runTimer); runTimer = null;
    await libRunEnded(kind, name, s);
  }, 900);
}

// A stopped run ends on "idle", the same phase a finished one reaches once the
// studio moves on, so job_cancelled is what tells them apart. Reading only the
// error phase announced a stopped run as done and went to open a version it
// never wrote.
async function libRunEnded(kind, name, s) {
  const stopped = !!s.job_cancelled && s.phase !== 'done' && s.phase !== 'error';
  // Recorded before the panel is rebuilt: the readout outlives the toast,
  // which is the whole point of it for a run that failed.
  const runError = s.phase === 'error' ? (s.message || t('lib.runFailed')) : '';
  // A summary that reached the answer limit is kept, and said to be cut short
  // where the readout will still show it after the toast has gone.
  const runNote = !runError && !stopped && s.summary_cut_short
    ? (s.message || t('sum.cutShort')) : '';
  setLib({runError, runNote});
  if (runError) toast(runError);
  else if (runNote) toast(runNote);
  else toast(t(stopped ? 'lib.runStopped' : 'lib.runDone'));
  if (!lib().current) return;
  await loadLibrary();
  const now = lib();
  if (!now.current) return;
  // Whatever was on screen is still there after a stop; stay on it.
  const landed = !runError && !stopped;
  openLibraryItem(now.current, landed && kind === 'transcribe' ? name : now.tx,
                  landed && kind === 'summarize' ? name : now.sum);
}

export async function stopLibRun() {
  const q = CANCEL_ASK.job;
  const go = await ask({title: t(q.title), body: t(q.body), yes: t(q.yes), no: t(q.no)});
  if (!go) return;
  // job_only: the run can finish while the question is on screen, and falling
  // back to a discard then would throw away the studio's take.
  const r = await post('/api/cancel', {job_only: true});
  if (r && r.error) { toast(r.error); return; }
  toast(t(r && r.stopped === 'job' ? 'toast.jobStopped' : 'toast.jobAlreadyDone'));
  refresh();
}

export async function openLibFolder() {
  const {current} = lib();
  if (!current) return;
  const r = await post('/api/library/open', {id: current});
  if (r.error) toast(r.error);
  else if (!r.ok) toast(t('toast.folderErr') + r.path);
}

// Deleting takes the folder off the disk, so it asks first and names what goes.
export async function deleteLibItem() {
  const {current: id, item, sessions} = lib();
  if (!id || !item) return;
  const s = sessions.find(x => x.id === id);
  const go = await ask({
    title: t('ask.deleteTitle'),
    body:  t('lib.deleteAsk', {name: s ? libDate(s) : id, path: item.path || id}),
    yes:   t('lib.delete'),
  });
  // Asking is not instant, so the selection may have moved under it -- and
  // deleting whatever happens to be selected now is not what was agreed to.
  if (!go || lib().current !== id) return;

  const r = await post('/api/library/delete', {id});
  if (r.error) { toast(r.error); return; }
  // Stop playback before the selection is dropped -- the <audio> still points
  // at a file that is gone.
  pauseLibraryAudio();
  setLib({current: null, item: null});
  toast(t('lib.deleted'));
  loadLibrary();
}
