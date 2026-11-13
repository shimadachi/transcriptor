import {useEffect, useLayoutEffect, useRef, useState} from 'react';

import {t} from '../i18n.js';
import {fmtBytes, jobFraction, libDate, plClock} from '../lib/format.js';
import {useApp} from '../lib/store.js';
import {PL_BARS, PL_GAP_GRACE, PL_RATES} from '../lib/tables.js';
import {closeLibRun, deleteLibItem, loadLibrary, openLibFolder, openLibRun, openLibraryItem,
        registerAudio, stopLibRun, submitLibRun} from '../state/library.js';
import {Select} from './Select.jsx';
import {jobProgress} from './Studio.jsx';
import {Summary} from './Summary.jsx';
import {TranscriptLines} from './Transcript.jsx';

const PL_FOLLOW_KEY = 'transcriptor-follow';

function LibraryList({sessions, current, loading}) {
  if (loading && !sessions.length) {
    return <div id="libList" className="lib-list"><span className="empty">{t('lib.loading')}</span></div>;
  }
  if (!sessions.length) {
    return <div id="libList" className="lib-list"><span className="empty">{t('lib.none')}</span></div>;
  }
  return (
    <div id="libList" className="lib-list">
      {sessions.map(s => {
        const tags = [];
        if (s.has_transcript) tags.push(t('lib.badges.tx'));
        if (s.has_summary) tags.push(t('lib.badges.sum'));
        if (s.audio) tags.push(t('lib.badges.aud') + ' · ' + fmtBytes(s.audio_bytes));
        return (
          <button key={s.id} type="button" className={'lib-row' + (s.id === current ? ' sel' : '')}
                  onClick={() => openLibraryItem(s.id)}>
            <span className="lib-when">{libDate(s)}</span>
            <span className="lib-prev">{s.preview || '—'}</span>
            <span className="lib-tags">{tags.map((x, i) => <i key={i}>{x}</i>)}</span>
          </button>
        );
      })}
    </div>
  );
}

// Where a re-run's output goes. Opens under the head when either re-run button
// is pressed, and only when there is something the run could overwrite.
function RunForm({kind, shown}) {
  const [mode, setMode] = useState('overwrite');
  const [name, setName] = useState('');
  return (
    <div id="libRun" className="lib-run">
      <div className="lib-run-q" id="libRunQ">
        {t(kind === 'transcribe' ? 'lib.runAskTx' : 'lib.runAskSum')}</div>
      <label className="lib-run-opt">
        <input type="radio" name="libRunMode" value="overwrite" checked={mode === 'overwrite'}
               onChange={() => setMode('overwrite')}/>
        <span id="libRunOver">
          {t(kind === 'transcribe' ? 'lib.runOverTx' : 'lib.runOverSum',
             {name: shown || t('lib.original')})}</span>
      </label>
      <label className="lib-run-opt">
        <input type="radio" name="libRunMode" value="new" checked={mode === 'new'}
               onChange={() => setMode('new')}/>
        <span>{t('lib.runNew')}</span>
        <input type="text" id="libRunName" maxLength={64} placeholder={t('lib.runNamePh')}
               value={name} onChange={e => setName(e.target.value)}/>
      </label>
      <div className="lib-run-go">
        <button id="libRunStart" className="mini ok"
                onClick={() => submitLibRun(mode, name)}>{t('lib.runStart')}</button>
        <button id="libRunCancel" className="ghost mini" onClick={closeLibRun}>{t('set.cancel')}</button>
      </div>
    </div>
  );
}

// What the models are doing. There is one job slot and the studio readout
// watches the same one, so progress is shown for whichever run holds it -- a
// re-run started here, or a take still being processed on the other tab, which
// is also why the buttons are dim. Once the worker is free the strip clears,
// except for a failure this tab asked for: that stays until the next run,
// because a toast is gone before it explains anything.
function RunStatus({s, runError, runNote}) {
  const failed = !s.processing && !!runError;
  const noted = !s.processing && !failed && !!runNote;
  if (!(s.processing || failed || noted)) return null;
  const progress = jobProgress({frac: jobFraction(s), pctId: 'libStatusPct',
                                trackId: 'libStatusTrack'});
  return (
    <div id="libStatus" role="status" aria-live="polite"
         className={'lib-status' + (failed ? ' err' : '') + (noted ? ' warn' : '')}>
      <div className="lib-status-line">
        <span className="lib-status-dot" aria-hidden="true"></span>
        <span id="libStatusMsg" className="lib-status-msg">
          {failed ? runError : noted ? runNote : (s.message || '')}</span>
        {progress.pct}
        {/* The one job slot is stopped from wherever its progress is shown. */}
        {s.processing && (
          <button id="libStop" className="mini danger" type="button" title={t('lib.stopTitle')}
                  onClick={stopLibRun}>{t('lib.stop')}</button>
        )}
      </div>
      {progress.track}
    </div>
  );
}

// The transport, the transcript it follows and the summary beside it. Mounted
// per recording (keyed by its id), so a new one starts at 0:00 with a clean
// index instead of inheriting the last one's.
function Detail({item, session, tx, sum, runKind, runError, runNote, busy, s}) {
  const audio = useRef(null);
  const track = useRef(null);
  const txBox = useRef(null);
  const [clock, setClock] = useState({cur: 0, dur: NaN});
  const [playing, setPlaying] = useState(false);
  const [muted, setMuted] = useState(false);
  const [rate, setRate] = useState(1);
  // Following is one feature -- light the line, keep it on screen -- and one
  // switch turns both halves off, because half of it is what makes the other
  // half worth having. On unless the reader has said otherwise, and remembered:
  // someone who reads at their own pace says so once.
  const [follow, setFollow] = useState(() => {
    try { return localStorage.getItem(PL_FOLLOW_KEY) !== '0'; } catch { return true; }
  });

  // The line being spoken is lit as the recording runs. Done on the elements
  // directly rather than through state: this runs several times a second, and
  // re-rendering an hour of transcript for one class would be felt.
  const lineEls = useRef([]);
  const lit = useRef(null);
  const scrolledAt = useRef(0);      // when the reader last scrolled by hand
  const followRef = useRef(follow);
  followRef.current = follow;

  const structured = !!(item.transcript && item.transcript.lines && item.transcript.lines.length);
  const seekable = structured && !!item.audio;
  const lines = seekable ? item.transcript.lines : null;

  // The last line that has started by `sec`, or null in a silence.
  const lineAt = sec => {
    if (!lines) return null;
    let lo = 0, hi = lines.length - 1, found = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      const start = typeof lines[mid].start === 'number' ? lines[mid].start : Infinity;
      if (start <= sec) { found = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (found < 0) return null;
    const l = lines[found];
    const end = typeof l.end === 'number' ? l.end : l.start;
    return sec <= end + PL_GAP_GRACE ? lineEls.current[found] || null : null;
  };

  // Bring the lit line back into view, but never fight someone who is reading
  // ahead: a scroll of their own buys a few seconds of being left alone.
  // `force` is the reader asking to be caught up -- switching following back
  // on -- which outranks the grace their last scroll bought them.
  const chase = (el, force) => {
    if (!force && Date.now() - scrolledAt.current < 4000) return;
    const box = txBox.current;
    if (!box || !el) return;
    const b = box.getBoundingClientRect(), r = el.getBoundingClientRect();
    if (!b.height) return;
    if (r.top >= b.top && r.bottom <= b.bottom) return;   // already on screen
    box.scrollTop += (r.top - b.top) - (b.height - r.height) / 2;
  };

  const highlight = force => {
    const a = audio.current;
    const next = (followRef.current && a && a.getAttribute('src'))
      ? lineAt(a.currentTime || 0) : null;
    if (next !== lit.current) {
      if (lit.current) lit.current.classList.remove('at');
      lit.current = next;
      if (next) next.classList.add('at');
      // Only chase the line while it is actually playing. Scrubbing or seeking
      // moves the highlight too, and yanking the panel around under a
      // stationary cursor is not helpful.
      if (next && a && !a.paused) chase(next, false);
    }
    if (force && lit.current) chase(lit.current, true);
  };

  const paint = () => {
    const a = audio.current;
    if (!a) return;
    setClock({cur: a.currentTime || 0, dur: a.duration});
    highlight(false);
  };

  useEffect(() => {
    const a = audio.current;
    registerAudio(a);
    const events = ['timeupdate', 'durationchange', 'loadedmetadata', 'seeked', 'emptied'];
    events.forEach(ev => a.addEventListener(ev, paint));
    const on = () => setPlaying(true), off = () => setPlaying(false);
    a.addEventListener('play', on);
    a.addEventListener('pause', off);
    a.addEventListener('ended', off);
    return () => {
      // Nothing keeps playing once its transport is gone.
      a.pause();
      registerAudio(null);
      events.forEach(ev => a.removeEventListener(ev, paint));
      a.removeEventListener('play', on);
      a.removeEventListener('pause', off);
      a.removeEventListener('ended', off);
    };
  }, []);

  // A different saved transcript is a different set of elements: the one that
  // was lit is gone, so light the right one in the new set. Layout, not a
  // plain effect, so it lands before the panel is painted.
  useLayoutEffect(() => {
    lineEls.current.length = lines ? lines.length : 0;
    // React reuses the elements where it can, so the class has to come off
    // by hand before the old line is forgotten.
    if (lit.current) lit.current.classList.remove('at');
    lit.current = null;
    highlight(false);
  }, [item.transcript_name, item.audio]);

  // Switching off clears the lit line; switching on lands on the moment the
  // recording is at, paused or not, rather than waiting for the next line.
  const toggleFollow = () => {
    const on = !follow;
    setFollow(on);
    followRef.current = on;
    try { localStorage.setItem(PL_FOLLOW_KEY, on ? '1' : '0'); } catch { /* no store */ }
    scrolledAt.current = 0;
    highlight(on);
  };

  const toggle = () => {
    const a = audio.current;
    if (!a.getAttribute('src')) return;
    // play() rejects when the src is gone or the format is refused; the button
    // follows the element's own events either way.
    if (a.paused) { const p = a.play(); if (p && p.catch) p.catch(() => {}); }
    else a.pause();
  };

  const seekAt = clientX => {
    const a = audio.current;
    if (!isFinite(a.duration) || a.duration <= 0) return;
    const r = track.current.getBoundingClientRect();
    a.currentTime = Math.min(1, Math.max(0, (clientX - r.left) / (r.width || 1))) * a.duration;
    paint();
  };

  // Clicking a timestamp moves the recording to it.
  const seekTo = sec => {
    const a = audio.current;
    if (!a.getAttribute('src') || !isFinite(sec)) return;
    const jump = () => {
      const dur = a.duration;
      a.currentTime = Math.max(0, isFinite(dur) && dur > 0 ? Math.min(sec, dur) : sec);
      paint();
      // Jumping to a moment is a request to hear it. Already playing, it just
      // moves; paused, it starts.
      if (a.paused) { const p = a.play(); if (p && p.catch) p.catch(() => {}); }
    };
    // preload="metadata" usually has the length in hand already; on the very
    // first click it may not, and seeking before then is ignored.
    if (a.readyState > 0) jump();
    else a.addEventListener('loadedmetadata', jump, {once: true});
  };
  const stampAt = target => {
    const el = target && target.closest && target.closest('.ts[data-at]');
    return el ? parseFloat(el.dataset.at) : NaN;
  };

  // Dragging keeps seeking after the pointer leaves the track, which is how
  // every other scrubber behaves and how a two-hour recording stays usable.
  const dragging = useRef(false);
  const drop = e => {
    dragging.current = false;
    try { track.current.releasePointerCapture(e.pointerId); } catch { /* not held */ }
  };
  const onTrackKey = e => {
    const a = audio.current;
    const step = e.shiftKey ? 30 : 5;      // Shift for a coarser jump
    const cur = a.currentTime || 0;
    if (e.key === 'ArrowRight') a.currentTime = isFinite(a.duration) ? Math.min(a.duration, cur + step) : cur + step;
    else if (e.key === 'ArrowLeft') a.currentTime = Math.max(0, cur - step);
    else if (e.key === 'Home') a.currentTime = 0;
    else if (e.key === 'End' && isFinite(a.duration)) a.currentTime = a.duration;
    else if (e.key === ' ' || e.key === 'Enter') toggle();
    else return;
    e.preventDefault();
    paint();
  };

  const known = isFinite(clock.dur) && clock.dur > 0;
  const p = known ? Math.min(1, Math.max(0, clock.cur / clock.dur)) : 0;
  const litBars = Math.round(p * PL_BARS);

  // The version pickers appear only once there is a choice: one transcript and
  // one summary is the normal state, and an empty dropdown beside each panel
  // would just be furniture.
  const variantOptions = list => (list || []).map(
    v => ({value: v.name, label: v.name || t('lib.original')}));
  const transcripts = item.transcripts || [], summaries = item.summaries || [];

  return (
    <div id="libBody">
      <div className="lib-head">
        <div className="lib-head-text">
          <div id="libTitle" className="lib-title">{session ? libDate(session) : item.id}</div>
          {/* The path lives inside a <bdi>, not directly in the <code>. The
              <code> is direction:rtl so the ellipsis lands at the front, where
              a truncated path can still be identified; the <bdi dir="ltr">
              isolates the text from that, so the leading slash stays a leading
              slash instead of being dragged to the far end. Either half alone
              is a bug. The title carries the whole path, since the element
              truncates. */}
          <code className="lib-path"><bdi id="libPath" dir="ltr" title={item.path || ''}>
            {item.path || ''}</bdi></code>
        </div>
        {/* Re-transcribing needs the audio and re-summarizing needs a
            transcript; neither can start while a job holds the only worker. */}
        <button id="libRetx" className="mini" title={t('lib.retxTitle')}
                disabled={busy || !item.audio}
                onClick={() => openLibRun('transcribe')}>{t('lib.retx')}</button>
        <button id="libResum" className="mini" title={t('lib.resumTitle')}
                disabled={busy || !transcripts.length}
                onClick={() => openLibRun('summarize')}>{t('lib.resum')}</button>
        <button id="libOpen" className="mini" onClick={openLibFolder}>{t('saved.open')}</button>
        <button id="libDelete" className="mini danger" title={t('lib.deleteTitle')}
                onClick={deleteLibItem}>{t('lib.delete')}</button>
      </div>

      {runKind && <RunForm key={runKind} kind={runKind}
                           shown={runKind === 'transcribe' ? tx : sum}/>}
      <RunStatus s={s} runError={runError} runNote={runNote}/>

      <div id="libAudioWrap" className="lib-audio" hidden={!item.audio}>
        {/* The element is the engine only; the transport below is the UI.
            preload=metadata so the length is on screen before playing. */}
        <audio id="libAudio" ref={audio} preload="metadata"
               src={item.audio ? '/api/library/audio?id=' + encodeURIComponent(item.id) : undefined}></audio>
        <div className="pl">
          <button id="plPlay" className={'pl-play' + (playing ? ' on' : '')}
                  title={t('pl.toggle')} onClick={toggle}></button>
          <div className="pl-main">
            <div id="plTrack" className="pl-track" role="slider" tabIndex={0} ref={track}
                 aria-valuemin={0} aria-valuemax={known ? Math.floor(clock.dur) : 0}
                 aria-valuenow={Math.floor(clock.cur)} aria-valuetext={plClock(clock.cur)}
                 title={t('pl.seek')}
                 onPointerDown={e => {
                   dragging.current = true;
                   try { track.current.setPointerCapture(e.pointerId); } catch { /* fine */ }
                   seekAt(e.clientX);
                 }}
                 onPointerMove={e => { if (dragging.current) seekAt(e.clientX); }}
                 onPointerUp={drop} onPointerCancel={drop} onKeyDown={onTrackKey}>
              {/* Ticks are real elements. Painted as a repeating gradient they
                  moiré into clumps: 1px hairlines on a 6px pitch do not survive
                  being laid over a few hundred fractional pixels. */}
              <div id="plBars" className="pl-bars" aria-hidden="true">
                {Array.from({length: PL_BARS}, (_, i) => (
                  <i key={i} className={i < litBars ? 'on' : undefined}></i>
                ))}
              </div>
              <div id="plHead" className="pl-head" style={{left: p * 100 + '%'}}></div>
            </div>
            <div className="pl-times">
              <span id="plNow" className="pl-now">{plClock(clock.cur)}</span>
              {/* "--:--" rather than 0:00 while the length is still unknown: a
                  zero there reads as an empty file. */}
              <span id="plDur" className="pl-dur">{known ? plClock(clock.dur) : '--:--'}</span>
            </div>
          </div>
          <button id="plMute" className={'pl-icon' + (muted ? ' off' : '')} title={t('pl.mute')}
                  onClick={() => {
                    const a = audio.current;
                    a.muted = !a.muted;
                    setMuted(a.muted);
                  }}>
            <svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true">
              <path d="M4 8h3l4-3.5v11L7 12H4z" fill="currentColor"/>
              <path className="pl-wave" d="M13.6 7.4a3.6 3.6 0 0 1 0 5.2" fill="none"
                    stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"/>
              <path className="pl-x" d="M13.8 7.8l4 4.4M17.8 7.8l-4 4.4" fill="none"
                    stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"/>
            </svg>
          </button>
          <button id="plRate" className={'pl-rate' + (rate !== 1 ? ' on' : '')} title={t('pl.rate')}
                  onClick={() => {
                    const a = audio.current;
                    const next = PL_RATES[(PL_RATES.indexOf(a.playbackRate) + 1) % PL_RATES.length];
                    a.playbackRate = next;
                    setRate(next);
                  }}>{rate}×</button>
          {/* Following the transcript: the line being spoken is lit and kept
              on screen. Its home is here because it only means anything while
              there is audio, and this whole row goes away when there is none. */}
          <button id="plFollowBtn" className={'pl-icon' + (follow ? '' : ' off')}
                  aria-pressed={follow} title={t('pl.follow')} onClick={toggleFollow}>
            <svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true">
              <path d="M3 5.5h9M3 14.5h9" fill="none" stroke="currentColor"
                    strokeWidth="1.6" strokeLinecap="round" opacity=".5"/>
              <path d="M3 10h6.5" fill="none" stroke="currentColor"
                    strokeWidth="1.6" strokeLinecap="round"/>
              <path className="pl-chase" d="M16 5.5v8m0 0l-2.3-2.4M16 13.5l2.3-2.4"
                    fill="none" stroke="currentColor" strokeWidth="1.6"
                    strokeLinecap="round" strokeLinejoin="round"/>
              <path className="pl-x" d="M13.6 12.4l4.8-4.8M13.6 7.6l4.8 4.8"
                    fill="none" stroke="currentColor" strokeWidth="1.6"
                    strokeLinecap="round"/>
            </svg>
          </button>
        </div>
        <div id="libAudioName" className="lib-audio-name">{item.audio || ''}</div>
      </div>
      <div id="libNoAudio" className="lib-none" hidden={!!item.audio}>{t('lib.noAudio')}</div>

      <div className="lib-cols">
        <section>
          <div className="tx-head"><span className="label">{t('tx.label')}</span>
            <Select id="libTxPick" className="lib-pick" title={t('lib.pickTx')}
                    hidden={transcripts.length <= 1} value={tx}
                    options={variantOptions(transcripts)}
                    onChange={v => openLibraryItem(item.id, v, sum)}/></div>
          {/* The reader's own scrolling and nothing else -- listening for
              'scroll' would also catch the follow's own, and the panel would
              freeze itself. */}
          <div id="libTranscript" className="tx-body" ref={txBox}
               onWheel={() => { scrolledAt.current = Date.now(); }}
               onTouchMove={() => { scrolledAt.current = Date.now(); }}
               onPointerDown={() => { scrolledAt.current = Date.now(); }}
               onClick={e => { const at = stampAt(e.target); if (!isNaN(at)) seekTo(at); }}
               onKeyDown={e => {
                 if (e.key !== 'Enter' && e.key !== ' ') return;
                 const at = stampAt(e.target);
                 if (isNaN(at)) return;
                 e.preventDefault();
                 seekTo(at);
               }}>
            {/* transcript.json keeps the speakers and timestamps, so it renders
                exactly like the studio's panel; the .txt is the fallback for a
                session saved before the JSON existed. */}
            {structured ? (
              <TranscriptLines res={item.transcript} seekable={seekable}
                               lineRef={(i, el) => { lineEls.current[i] = el; }}/>
            ) : item.transcript_text ? (
              <div className="lib-raw">{item.transcript_text}</div>
            ) : (
              <span className="empty">{t('lib.noTx')}</span>
            )}
          </div>
        </section>
        <section>
          <div className="tx-head"><span className="label">{t('sum.label')}</span>
            <Select id="libSumPick" className="lib-pick" title={t('lib.pickSum')}
                    hidden={summaries.length <= 1} value={sum}
                    options={variantOptions(summaries)}
                    onChange={v => openLibraryItem(item.id, tx, v)}/></div>
          <Summary id="libSummary" text={item.summary} emptyText={t('lib.noSum')}/>
        </section>
      </div>
    </div>
  );
}

export function Library({hidden}) {
  const st = useApp();
  const lib = st.lib;
  const s = st.s || {};
  const session = lib.item ? lib.sessions.find(x => x.id === lib.item.id) : null;
  return (
    <div id="viewLibrary" className="view" role="tabpanel" aria-labelledby="tabLibrary"
         hidden={hidden}>
      <section className="panel">
        <div className="tx-head">
          <span className="label">{t('lib.label')}</span>
          {/* direction:rtl on the <code> with a <bdi dir="ltr"> inside: see the
              note on the recording's own path below. Either half alone is a
              bug. */}
          <code className="lib-dir"><bdi id="libDir" dir="ltr" title={lib.dir}>{lib.dir}</bdi></code>
          <button id="libRefresh" className="mini" title={t('lib.refresh')}
                  onClick={loadLibrary}>⟳</button>
        </div>

        <div className="lib-grid">
          <LibraryList sessions={lib.sessions} current={lib.current} loading={lib.loading}/>
          <div id="libDetail" className="lib-detail">
            {lib.item ? (
              <Detail key={lib.item.id} item={lib.item} session={session}
                      tx={lib.tx} sum={lib.sum} runKind={lib.runKind}
                      runError={lib.runError} runNote={lib.runNote}
                      busy={!!s.processing} s={s}/>
            ) : (
              <div id="libPlaceholder" className="lib-placeholder">
                <span className="empty">{t('lib.pick')}</span>
              </div>
            )}
          </div>
        </div>
      </section>
    </div>
  );
}
