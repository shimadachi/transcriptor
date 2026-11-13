// Takes recorded in this window (microphone and/or system audio through the
// page's own media APIs) and uploaded to the same offline pipeline. The
// engine's own devices are preferred wherever it has them; this is what works
// everywhere, including where the engine cannot reach an audio device at all.
//
// The flags the rest of the interface reads are published into the store
// (`capture`, `pending`). Everything else here is the recorder's own business.
import {upload} from '../lib/api.js';
import {app} from '../lib/store.js';
import {t} from '../i18n.js';
import {ask, toast} from './ui.js';

// `starting` covers the gap between the click and the permission answer.
// `rec` cannot: it is only true once the user has granted, and the poll puts
// the Record button back within 700ms because the *engine* is idle -- so a
// second click during the prompt started a second capture over the first.
// `uploading` covers the handoff after the capture is torn down but before
// the engine has the take: `rec` is already false there, so polling would
// otherwise re-enable Record and let a second take start on top of the upload.
let rec = false, starting = false, paused = false, uploading = false;
let cancelled = false;
// Whether the current take has already been handed off. Both the recorder's
// stop event and the error fallback can arrive; only the first may act.
let finalized = false;
// Which take the recorder callbacks belong to. `finalized` alone is reset by
// every new capture, so a fallback timer left over from an errored take found
// it false again and finished the take after it: the new recording was cut off
// mid-sentence, its partial chunks uploaded as a whole take, and the rest lost
// because the real stop event then found the work already done.
let takeId = 0, stopTimer = null;
// How long the error fallback waits for a stop event that may never come.
const STOP_FALLBACK_MS = 2000;
let startTs = 0, pausedMs = 0, pauseTs = 0;
let recorder = null, chunks = [], streams = [], actx = null, analyser = null, abuf = null;

// Called after anything here changes what the transport should show. Set by
// state/studio.js to its poll, which is also what reads this module -- wiring
// it this way keeps the two from importing each other in a circle.
let refresh = () => {};
export function onCaptureChange(fn) { refresh = fn; }

function publish() {
  app.set({capture: {rec, starting, paused, uploading}});
}

export const captureState = () => ({rec, starting, paused, uploading});

export function captureElapsed() {
  if (!startTs) return 0;
  const extra = paused ? (performance.now() - pauseTs) : 0;
  return (performance.now() - startTs - pausedMs - extra) / 1000;
}

export function captureLevel() {
  if (!analyser || !abuf) return 0;
  analyser.getByteTimeDomainData(abuf);
  let sum = 0;
  for (let i = 0; i < abuf.length; i++) { const v = (abuf[i] - 128) / 128; sum += v * v; }
  return Math.sqrt(sum / abuf.length);
}

function pickMime() {
  const candidates = ['audio/webm;codecs=opus', 'audio/webm', 'audio/ogg;codecs=opus'];
  for (const type of candidates) {
    if (window.MediaRecorder && MediaRecorder.isTypeSupported(type)) return type;
  }
  return '';
}

function cleanupStreams() {
  streams.forEach(s => s.getTracks().forEach(track => track.stop()));
  streams = [];
  if (actx) { try { actx.close(); } catch { /* already closed */ } actx = null; }
  analyser = null; abuf = null;
}

// `kind` is 'mic', 'system' or 'both'.
export async function startCapture(kind) {
  if (rec || starting || uploading) return;
  // A retained take is the only copy of a recording. Starting another one would
  // hand the slot to the new take and quietly drop the old, so make the user
  // resolve it first -- retry, save a copy, or discard.
  if (pendingBlob) { toast(t('toast.resolvePending')); return; }
  if (!navigator.mediaDevices || !window.MediaRecorder) {
    toast(t('toast.noBrowserRec'));
    return;
  }
  // Set synchronously, before the first await, so a second click cannot get
  // past the guard above while this one waits on the permission prompt.
  starting = true;
  publish();

  // This attempt's streams stay local until it becomes the live capture. Put
  // straight into the shared list, a second attempt's reset orphaned the first
  // one's tracks with the microphone still open.
  const mine = [];
  const drop = () => mine.forEach(s => s.getTracks().forEach(track => track.stop()));
  let ctx = null;
  // Only the very last statement sets this. Everything before it is an attempt
  // that has to leave nothing behind, whichever way it ends.
  let live = false;
  try {
    try {
      if (kind === 'mic' || kind === 'both') {
        mine.push(await navigator.mediaDevices.getUserMedia(
          {audio: {echoCancellation: false, noiseSuppression: false}}));
      }
      if (kind === 'system' || kind === 'both') {
        const ds = await navigator.mediaDevices.getDisplayMedia({video: true, audio: true});
        ds.getVideoTracks().forEach(track => track.stop());  // we only want the audio
        mine.push(ds);
      }
    } catch { toast(t('toast.noPermission')); return; }

    // Mix every captured stream into one track (mic + system together).
    ctx = new (window.AudioContext || window.webkitAudioContext)();
    const dest = ctx.createMediaStreamDestination();
    let hasAudio = false;
    mine.forEach(s => {
      if (s.getAudioTracks().length) {
        ctx.createMediaStreamSource(s).connect(dest); hasAudio = true;
      }
    });
    if (!hasAudio) { toast(t('toast.noAudio')); return; }

    // Past every await and every way out: this attempt is the capture now, so
    // publish its resources into the variables the rest of the module works with.
    streams = mine; actx = ctx;
    analyser = actx.createAnalyser(); analyser.fftSize = 512;
    abuf = new Uint8Array(analyser.fftSize);
    actx.createMediaStreamSource(dest.stream).connect(analyser);

    chunks = [];
    const mime = pickMime();
    recorder = new MediaRecorder(dest.stream, mime ? {mimeType: mime} : undefined);
    recorder.ondataavailable = e => { if (e.data && e.data.size) chunks.push(e.data); };
    finalized = false;
    // Every callback below names the take it was set up for, so nothing this
    // recorder says can be acted on once a later one holds the microphone.
    const take = ++takeId;
    recorder.onstop = () => onCaptureStop(take);
    // An encoder that gives up used to be silent: no onstop, `rec` stuck true,
    // streams still open, and the only way out a reload.
    //
    // Report it, but do NOT finish the take here. The recording spec ends an
    // errored recording the ordinary way -- a final `dataavailable`, then `stop`
    // -- so finalizing on the error as well uploaded the chunks captured so far
    // as one take and the final chunk as a second, competing one. Let onstop do
    // the handoff; the timer is only for a recorder that sends no stop event.
    recorder.onerror = () => {
      if (finalized || take !== takeId) return;
      toast(t('toast.recError'));
      clearTimeout(stopTimer);
      stopTimer = setTimeout(() => onCaptureStop(take), STOP_FALLBACK_MS);
    };

    // The recorder is fed by the AudioContext destination, whose track never
    // ends -- so a source that stopped being shared was invisible here and the
    // page went on cheerfully recording silence. Watch the real sources.
    mine.forEach(s => s.getAudioTracks().forEach(track => {
      track.addEventListener('ended', () => {
        if (!rec) return;
        toast(t('toast.sourceEnded'));
        stopCapture();   // keeps the take; onstop uploads it
      });
    }));

    recorder.start(1000);
    rec = true; paused = false; cancelled = false;
    startTs = performance.now(); pausedMs = 0; pauseTs = 0;
    live = true;
  } catch {
    // Everything after the permission prompt -- the AudioContext, the audio
    // graph, the MediaRecorder constructor, start() -- used to fall straight
    // through the finally below, which only lowered `starting`. The microphone
    // stayed open, invisibly, until the window was reloaded.
    toast(t('toast.captureFailed'));
  } finally {
    if (!live) {
      drop();
      if (ctx) { try { ctx.close(); } catch { /* already closed */ } }
      // The shared variables may already point at this attempt's resources;
      // drop() and close() above have dealt with the objects, so just let go.
      recorder = null; chunks = [];
      streams = []; actx = null; analyser = null; abuf = null;
      rec = false;
    }
    starting = false;
    publish();
  }
  refresh();
}

export function toggleCapturePause() {
  if (!recorder) return;
  if (paused) {
    recorder.resume(); pausedMs += performance.now() - pauseTs; paused = false;
  } else {
    recorder.pause(); pauseTs = performance.now(); paused = true;
  }
  publish();
}

export function stopCapture() {
  if (recorder && recorder.state !== 'inactive') recorder.stop();
}

export function cancelCapture() {
  cancelled = true;   // onCaptureStop will discard without uploading
  stopCapture();
}

// A take that has been captured but not yet accepted by the engine. From the
// moment the capture is torn down this Blob is the only copy in existence, so
// it is held here -- with something on screen to press -- until the engine has
// it or the user says to let it go. Kept in a local variable it went out of
// scope the instant the upload failed: a busy engine turned an hour of meeting
// into a two-second toast.
// pendingId identifies which take currently occupies the slot. An upload only
// clears the slot if it is still the one it was given, because "the upload
// finished" and "the take on screen is that upload's" are different questions:
// a retry that completed after a newer take had replaced it used to clear the
// newer one, throwing away a recording the engine had never seen.
let pendingBlob = null, pendingName = '', pendingId = 0;

function clearPending() {
  pendingBlob = null; pendingName = '';
  pendingId += 1;            // whatever held the slot no longer does
  app.set({pending: null});
}

// Hands the take to the pipeline. Returns whether the engine took it; keeps the
// Blob and raises the retry row when it did not. Busy for its whole duration --
// including from Retry, which used to leave the transport open and let a second
// recording start on top of an upload in flight.
async function uploadRecording(blob, name) {
  pendingBlob = blob; pendingName = name;
  const mine = ++pendingId;
  uploading = true;
  publish();
  let message = '';
  try {
    const r = await upload(blob, name);
    if (!r.error) {
      // Only clear if this upload still owns the slot.
      if (pendingId === mine) clearPending();
      return true;
    }
    message = r.error;
  } catch { message = t('toast.uploadErr'); }
  finally { uploading = false; publish(); }
  toast(message);
  if (pendingId === mine) app.set({pending: {message}});
  return false;
}

export async function retryPending() {
  if (!pendingBlob || uploading) return;
  app.set({pendingRetrying: true});
  toast(t('toast.processing'));
  try { await uploadRecording(pendingBlob, pendingName); }
  finally { app.set({pendingRetrying: false}); refresh(); }
}

// The way out that does not depend on the engine working at all.
export function savePending() {
  if (!pendingBlob) return;
  const url = URL.createObjectURL(pendingBlob);
  const a = document.createElement('a');
  a.href = url; a.download = pendingName;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

export async function dropPending() {
  if (!pendingBlob) return;
  const go = await ask({
    title: t('ask.discardTitle'),
    body:  t('pending.dropAsk'),
    yes:   t('ask.discard'),
  });
  // The take could have been resolved while the question was on screen.
  if (!go || !pendingBlob) return;
  clearPending();
  toast(t('toast.pendingDropped'));
}

async function onCaptureStop(take) {
  // Only the take that is still live may be finished.
  if (take !== undefined && take !== takeId) return;
  // Idempotent: a take is handed off exactly once. Both the stop event and the
  // error fallback lead here, and running twice split one recording across two
  // uploads -- the second carrying only whatever arrived after the first ran.
  if (finalized) return;
  finalized = true;
  // Nothing is waiting for a stop event any more. Left scheduled, this is the
  // timer that used to come back two seconds later and end the next recording.
  clearTimeout(stopTimer); stopTimer = null;

  rec = false; paused = false;
  const type = (chunks[0] && chunks[0].type) || 'audio/webm';
  const blob = new Blob(chunks, {type}); chunks = [];
  cleanupStreams();
  publish();
  if (cancelled) { cancelled = false; toast(t('toast.recCancelled')); refresh(); return; }
  if (!blob.size) { toast(t('toast.emptyRec')); refresh(); return; }
  const ext = type.includes('ogg') ? 'ogg' : 'webm';
  // Busy across the handoff: `rec` is already false, so without this the next
  // poll would put Record back within 700ms and a second take could start on
  // top of the multipart upload still in flight.
  uploading = true;
  publish();
  toast(t('toast.processing'));
  try {
    await uploadRecording(blob, 'recording.' + ext);
  } finally {
    uploading = false;
    publish();
  }
  refresh();
}
