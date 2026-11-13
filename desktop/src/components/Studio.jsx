import {useLayoutEffect, useMemo, useRef} from 'react';

import {t} from '../i18n.js';
import {jobFraction, pctText} from '../lib/format.js';
import {app, useApp} from '../lib/store.js';
import {BROWSER_SOURCES, METER_BARS} from '../lib/tables.js';
import {dropPending, retryPending, savePending} from '../state/capture.js';
import {fetchModel, loadSources, openOutputFolder, pickTemplate, pressCancel, pressPause,
        pressRecord, pressSummarize, pressTranscribe, processFile, toggleLive,
        transport} from '../state/studio.js';
import {openSettings} from '../state/settings.js';
import {Select} from './Select.jsx';
import {Summary} from './Summary.jsx';
import {TranscriptLines, hasLines} from './Transcript.jsx';

// One readout, drawn twice: the studio console and the library strip show the
// same job from the same poll.
export function jobProgress({frac, pctId, trackId}) {
  return {
    // The percentage moves every tick, which is no use read out loud.
    pct: <span id={pctId} className="job-pct" aria-hidden="true">
           {frac == null ? '' : pctText(frac)}</span>,
    track: (
      <div id={trackId} className="job-track" hidden={frac == null} aria-hidden="true">
        <i style={frac == null ? undefined : {width: Math.round(frac * 100) + '%'}}></i>
      </div>
    ),
  };
}

function Meter({live, level, rec}) {
  // Each bar gets its own rhythm once, so the meter does not pulse in unison.
  const bars = useMemo(() => Array.from({length: METER_BARS}, () => ({
    animationDelay: (Math.random() * 0.9).toFixed(2) + 's',
    animationDuration: (0.7 + Math.random() * 0.6).toFixed(2) + 's',
  })), []);
  const amp = Math.max(0.28, Math.min(1, level * 11));
  return (
    <div className={'meter' + (live ? ' live' : '')} id="meter"
         style={{transform: rec ? 'scaleY(' + amp + ')' : 'scaleY(1)'}}>
      {bars.map((style, i) => <i key={i} style={style}></i>)}
    </div>
  );
}

// First-run model notices, each a way in rather than a line of text to read
// and wonder what to do with. The speech ones open the panel that picks a
// model, because there is a choice to make; the speaker models and the voice
// detector have no choice attached, so those just fetch and report here.
//
// These five strings carry their own <b>: they are this app's text from
// i18n.js, never anything the engine or a model wrote, which is the only
// reason they may be set as HTML.
const marked = key => ({dangerouslySetInnerHTML: {__html: t(key)}});

function Notices({s, capturing}) {
  const notes = [];
  const dl = s.model_download || {};
  const go = fn => e => { e.preventDefault(); fn(); };

  // A download the caution started itself: busy, failed, or not begun.
  const fetchable = (kind, missingKey) => {
    if (dl.active && dl.kind === kind) {
      const pct = dl.progress == null ? '' : pctText(dl.progress);
      return <span className="caution busy">{(dl.message || t('llm.downloading')) + pct}</span>;
    }
    if (dl.kind === kind && dl.error && !dl.cancelled) {
      return <a className="caution err" onClick={go(() => fetchModel(kind))}>
               {dl.error + t('note.diarRetry')}</a>;
    }
    return <a className="caution" onClick={go(() => fetchModel(kind))} {...marked(missingKey)}></a>;
  };

  // First, and while it can still be fixed: the take is running and nothing is
  // reaching it. The text comes from the engine already in the UI language.
  if (!capturing && s.record_notice) {
    notes.push(<span className="caution warn">{s.record_notice}</span>);
  }
  if (!s.stt_cached) {
    notes.push(<a href="#settings" className="caution"
                  onClick={go(() => openSettings('s_model'))} {...marked('note.sttMissing')}></a>);
  }
  // Only while Live is on: a model nobody asked to use is not missing.
  const lv = s.live || {};
  if (lv.on && !lv.ready) {
    notes.push(<a href="#settings" className="caution"
                  onClick={go(() => openSettings('s_livemodel'))}
                  {...marked('note.liveMissing')}></a>);
  }
  if (s.diarization_enabled && s.diar_supported && !s.diar_cached) {
    notes.push(fetchable('diarize', 'note.diarMissing'));
  }
  if (s.diarization_enabled && !s.diar_supported) {
    notes.push(<span {...marked('note.diarUnbuilt')}></span>);
  }
  // The voice detector is never fetched by a transcription, so this is how an
  // install that predates it gets it. Only once a speech model is here: until
  // then that model's own caution comes first, and its download brings the
  // detector along.
  if (s.stt_cached && s.vad_cached === false) notes.push(fetchable('vad', 'note.vadMissing'));

  if (!notes.length) return null;
  return (
    <div id="diarHint" className="hint" style={{display: 'block'}}>
      {notes.map((n, i) => <span key={i}>{i > 0 && <br/>}{n}</span>)}
    </div>
  );
}

function Console() {
  const st = useApp();
  const fileInput = useRef(null);
  const s = st.s || {};
  const {rec, paused, elapsed, level} = st.view;
  const cap = st.capture;
  const rules = transport(st);
  const progress = jobProgress({frac: jobFraction(s), pctId: 'statusPct', trackId: 'statusTrack'});

  const sourceOptions = BROWSER_SOURCES.map(b => ({value: b.id, label: t(b.key)}))
    .concat(st.sources.map(x => ({value: x.id, label: x.label,
                                  icon: x.is_loopback ? 'out' : 'mic'})));
  const mics = st.sources.filter(x => !x.is_loopback);
  const micOptions = [{value: '', label: t(mics.length ? 'src.micNoneAvail' : 'src.micNone')}]
    .concat(mics.map(m => ({value: m.id, label: '+ ' + m.label})));
  // The "+ mic" mixer only applies to the engine's own loopback capture; the
  // window's sources do their own mic+system mixing.
  const showMic = !st.source.startsWith('browser:') && mics.length > 0;

  const status = !st.s ? t('src.ready')
    : cap.rec ? t(paused ? 'st.browserPaused' : 'st.browserRec') : s.message;

  return (
    <section className="panel">
      <div className="console">
        <div className={'rec-wrap' + (rec ? ' on' : '')} id="recWrap">
          <span className="ring"></span><span className="ring d2"></span>
          {/* The disc is the whole transport: the dot morphs into a square
              while recording, and pressing it again stops. */}
          <button className="rec-btn" id="recBtn" title={t(rec ? 'rec.stop' : 'rec.start')}
                  aria-pressed={rec} disabled={rules.recDisabled} onClick={pressRecord}></button>
        </div>
        <div className="console-main">
          <div className="src-row">
            <span className="label">{t('src.label')}</span>
            <Select id="source" grow value={st.source} options={sourceOptions}
                    disabled={rules.busy} onChange={v => app.set({source: v})}/>
            <Select id="micSource" value={st.mic} options={micOptions} hidden={!showMic}
                    disabled={rules.busy} title={t('src.micMix')}
                    onChange={v => app.set({mic: v})}/>
            <button id="refreshSrc" className="mini" title={t('src.refresh')}
                    disabled={rules.busy} onClick={loadSources}>⟳</button>
            <button id="fileBtn" className="mini" title={t('src.fileTitle')}
                    disabled={rules.busy}
                    onClick={() => fileInput.current.click()}>{t('src.file')}</button>
            <button id="pauseBtn" className={'mini' + (paused ? ' on' : '')}
                    title={t('src.pauseTitle')} disabled={rules.pauseDisabled}
                    onClick={pressPause}>{t(paused ? 'src.resume' : 'src.pause')}</button>
            <button id="cancelBtn" className="mini danger"
                    title={s.processing ? t('btn.cancelJob') : t('src.cancelTitle')}
                    disabled={rules.cancelDisabled} onClick={pressCancel}>{t('src.cancel')}</button>
            <input ref={fileInput} type="file" id="fileInput" accept="audio/*,video/*" hidden
                   onChange={async e => {
                     const input = e.target;
                     await processFile(input.files[0]);
                     input.value = '';
                   }}/>
          </div>
          <Meter live={rec && !paused} level={level} rec={rec}/>
          <div className="readout">
            {/* The percentage qualifies the phase, so it goes with it and the
                caret stays at the end of the line. */}
            <span id="statusMsg">{status}</span>{progress.pct}<span
                  id="spinner" className={'cursor' + (s.processing ? ' on' : '')}></span>
            {' '}<span id="elapsed">
              {rec ? elapsed.toFixed(1) + 's' + (paused ? t('st.pausedSuffix') : '') : ''}</span>
          </div>
          {progress.track}
        </div>
      </div>
      {st.s && <Notices s={s} capturing={cap.rec}/>}
      {/* What could not be written. Stays up until the next take: a recording
          that only exists in memory is one the user loses by closing the
          window, so this must not be a toast that scrolls away unseen. */}
      {s.save_error && (
        <div id="saveErr" className="saved save-err" style={{display: 'flex'}}>
          <span className="saved-k">{t('saved.failed')}</span>
          <code id="saveErrMsg">{s.save_error}</code>
        </div>
      )}
      {/* A take recorded here that the engine has not accepted. It is the only
          copy the moment the capture is torn down, so it stays, with something
          to press, until it lands or the user lets it go. */}
      {st.pending && (
        <div id="pendingRow" className="saved save-err" style={{display: 'flex'}}>
          <span className="saved-k">{t('pending.k')}</span>
          <code id="pendingMsg">{st.pending.message}</code>
          <button id="pendingRetry" className="mini" disabled={st.pendingRetrying}
                  onClick={retryPending}>{t('pending.retry')}</button>
          <button id="pendingSave" className="mini" onClick={savePending}>{t('pending.save')}</button>
          <button id="pendingDrop" className="mini" onClick={dropPending}>{t('pending.drop')}</button>
        </div>
      )}
      {s.output_dir && (
        <div id="savedRow" className="saved" style={{display: 'flex'}}>
          {/* A folder that exists is not a take that was saved. With a write
              failed, "Saved" beside "Not saved" contradicted itself about the
              same folder, so the row then only says where the folder is. */}
          <span id="savedK" className="saved-k">{t(s.save_error ? 'saved.folder' : 'saved.k')}</span>
          <code id="savedPath">{s.output_dir}</code>
          <button id="openFolder" className="mini" onClick={openOutputFolder}>{t('saved.open')}</button>
        </div>
      )}
    </section>
  );
}

// The live transcript's switch. On and off differ in the icon, not only in
// colour: the broadcast waves are struck through while it is off, the way the
// player's mute is.
function LiveButton({lv, rec}) {
  const on = !!lv.on;
  // The waves move only while a take is actually being heard. On between
  // takes, or on with no model to run, is on and still.
  const hearing = on && !!lv.active && !!rec;
  return (
    <button id="liveBtn" type="button" aria-pressed={on}
            className={'mini live-btn ' + (on ? 'on' : 'off') + (hearing ? ' hearing' : '')}
            title={t(on ? 'live.onTitle' : 'live.offTitle')} onClick={toggleLive}>
      <svg viewBox="0 0 20 20" width="16" height="16" aria-hidden="true">
        <defs>
          {/* Cuts a gap around the slash, so it reads as struck through on any
              background rather than as one more line among the waves. */}
          <mask id="liveCut" maskUnits="userSpaceOnUse" x="0" y="0" width="20" height="20">
            <rect width="20" height="20" fill="#fff"/>
            <path className="live-cut" d="M3.2 3.2l13.6 13.6" stroke="#000"
                  strokeWidth="4.4" strokeLinecap="round"/>
          </mask>
        </defs>
        <g mask="url(#liveCut)">
          <circle className="live-dot" cx="10" cy="10" r="2.1" fill="currentColor"/>
          <path className="live-wave" d="M7.1 6.9a4.4 4.4 0 0 0 0 6.2M12.9 6.9a4.4 4.4 0 0 1 0 6.2"
                fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"/>
          <path className="live-wave w2" d="M4.6 4.4a7.9 7.9 0 0 0 0 11.2M15.4 4.4a7.9 7.9 0 0 1 0 11.2"
                fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"/>
        </g>
        <path className="live-slash" d="M3.2 3.2l13.6 13.6" fill="none" stroke="currentColor"
              strokeWidth="1.7" strokeLinecap="round"/>
      </svg>
      <span>{t('live.btn')}</span>
    </button>
  );
}

function LiveLine({l, partial}) {
  return (
    <div className={partial ? 'line partial' : 'line'}>
      {l.ts && <span className="ts">{l.ts}</span>}
      <span className="txt">{l.text}</span>
    </div>
  );
}

function TranscriptPanel() {
  const st = useApp();
  const s = st.s || {};
  const {rec} = st.view;
  const rules = transport(st);
  const lv = s.live || {};
  const box = useRef(null);
  // Whether the reader was at the bottom before the preview grew: someone
  // scrolled up to read back is not dragged down by every new line.
  const atBottom = useRef(true);

  useLayoutEffect(() => {
    const el = box.current;
    if (el && st.live && atBottom.current) el.scrollTop = el.scrollHeight;
  }, [st.live]);

  // The preview's head and the note under its lines follow the session's state
  // and the interface language, neither of which moves the text's revision.
  let liveState = '', note = '', noteErr = false;
  if (st.live) {
    if (lv.active) {
      liveState = t(lv.state === 'loading' ? 'live.loading'
                  : lv.state === 'finishing' ? 'live.finishing' : 'live.listening');
    } else if (lv.has_text) {
      liveState = t('live.preview');
    }
    if (rec && st.capture.rec) note = t('live.browser');
    else if (lv.error) { note = lv.error; noteErr = true; }
    else if (rec && lv.on && !lv.ready) note = t('live.noModel');
    else if (lv.lagging) note = t('live.lagging');
  }

  return (
    <section className="panel">
      <div className="tx-head">
        <span className="label">{t('tx.label')}</span>
        <LiveButton lv={st.liveOverride || lv} rec={rec && !st.liveOverride}/>
        <button id="txBtn" className="ok" disabled={!st.s || rules.txDisabled}
                title={!st.s || s.stt_cached ? t('tx.goTitle') : t('note.sttBtn')}
                onClick={pressTranscribe}>{t('tx.go')}</button>
      </div>
      {/* Says the panel holds the live preview, not the finished transcript.
          Outside the scroller, so it never scrolls away under the lines. */}
      <div id="liveHead" className={'live-head' + (lv.active ? '' : ' done')} hidden={!st.live}>
        <span id="livePill" className="live-pill">{t('live.now')}</span>
        <span id="liveState">{liveState}</span>
      </div>
      <div id="transcript" className="tx-body" ref={box}
           onScroll={e => {
             const el = e.currentTarget;
             atBottom.current = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
           }}>
        {st.live ? (
          <>
            <div>
              {st.live.lines.map((l, i) => <LiveLine key={i} l={l}/>)}
              {st.live.partial && <LiveLine l={st.live.partial} partial/>}
            </div>
            <div className={'live-msg' + (noteErr ? ' err' : '')} hidden={!note}>{note}</div>
          </>
        ) : hasLines(st.result) ? (
          <TranscriptLines res={st.result}/>
        ) : (
          <span className="empty">{t(st.txEmptyKey)}</span>
        )}
      </div>
    </section>
  );
}

function SummaryPanel() {
  const st = useApp();
  const s = st.s || {};
  const rules = transport(st);
  const ctx = st.ctx;
  const setCtx = patch => app.set({ctx: {...ctx, ...patch}});
  return (
    <section className="panel">
      <div className="tx-head">
        <span className="label">{t('sum.label')}</span>
        <Select id="tpl" value={st.tpl} title={t('sum.tplTitle')} onChange={pickTemplate}
                options={st.templates.map(x => ({value: x.value, label: x.label}))}/>
        <button id="ctxBtn" className={'mini' + (ctx.open ? ' on' : '')} title={t('sum.ctxTitle')}
                onClick={() => setCtx({open: !ctx.open})}>{t('sum.ctx')}</button>
        {/* A dimmed button says why on hover, so it is not a dead end. */}
        <button id="sumBtn" className="ok" disabled={!st.s || rules.sumDisabled}
                title={st.s && rules.sumNoWords ? t(s.has_result ? 'sum.emptyTx' : 'sum.noTx') : ''}
                onClick={pressSummarize}>{t('sum.go')}</button>
      </div>
      <div id="ctxBox" className="ctx" style={{display: ctx.open ? 'grid' : 'none'}}>
        <input type="text" id="ctxTitle" placeholder={t('sum.phTitle')} value={ctx.title}
               onChange={e => setCtx({title: e.target.value})}/>
        <input type="text" id="ctxPeople" placeholder={t('sum.phPeople')} value={ctx.people}
               onChange={e => setCtx({people: e.target.value})}/>
        <textarea id="ctxNotes" rows="2" placeholder={t('sum.phNotes')} value={ctx.notes}
                  onChange={e => setCtx({notes: e.target.value})}></textarea>
      </div>
      <Summary id="summary" text={st.summary}/>
      {/* Under a summary the answer limit stopped mid-sentence, for as long as
          that summary is the one on screen. */}
      <div id="sumCut" className="sum-cut" hidden={!(st.summary && st.summaryCut)}>
        {t('sum.cutShort')}</div>
    </section>
  );
}

export function Studio({hidden}) {
  return (
    <div id="viewStudio" className="view" role="tabpanel" aria-labelledby="tabStudio"
         hidden={hidden}>
      <Console/>
      <div className="workspace">
        <TranscriptPanel/>
        <SummaryPanel/>
      </div>
    </div>
  );
}
