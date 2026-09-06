// Regression tests for the browser-side recording path, run against the real
// web/app.js. Every case here corresponds to a bug that shipped:
//
//   R3  a failed upload dropped the only copy of the take
//   R10 a capture that failed after the permission prompt left the mic open
//   R13 one failed /api/result left the panels stale for ever
//   G1  a second take, or an overlapping retry, destroyed a retained take
//   G2  a recorder error uploaded one take as two
//   G3  slow /api/result responses starved every render
//   G4  an errored take's fallback timer terminated the take after it
//   G5  overwriting a named version overwrote the session's original instead

const path = require('path');
const {createEnv} = require('./harness');

const ROOT = process.argv[2] || path.join(__dirname, '..', '..');

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'}  ${name}${detail ? '  --  ' + detail : ''}`);
  if (!ok) failures++;
}

async function run() {
  // ---------------------------------------------------------------- R10 ----
  // Every stage after the permission prompt must release what it acquired.
  for (const stage of ['actx', 'mr', 'start']) {
    const env = createEnv(ROOT);
    env.media.failAt = stage;
    await env.app.startBrowserCapture('both');
    await env.settle();
    const expectClose = stage !== 'actx';   // 'actx' fails before one exists
    check(`R10 ${stage}: capture tracks are stopped`, env.media.stopped > 0,
          `${env.media.stopped} stop() calls`);
    check(`R10 ${stage}: the AudioContext is closed`,
          expectClose ? env.media.ctxClosed === 1 : env.media.ctxClosed === 0,
          `${env.media.ctxClosed} close() calls`);
    check(`R10 ${stage}: the page is not left recording`,
          env.peek('browserRec') === false && env.peek('browserStarting') === false);
  }
  {
    const env = createEnv(ROOT);
    await env.app.startBrowserCapture('both');
    await env.settle();
    check('a capture with nothing wrong still starts', env.peek('browserRec') === true);
  }

  // ----------------------------------------------------------------- R3 ----
  {
    const env = createEnv(ROOT);
    env.server.uploadFails = true;
    env.poke('_chunks = [{type: "audio/webm", size: 10}]; _cancelled = false;');
    await env.app.onBrowserStop();
    await env.settle();
    check('R3 a failed upload keeps the take', env.peek('_pendingBlob') !== null);
    check('R3 the recovery row is shown',
          env.els.pendingRow.style.display === 'flex');

    env.server.uploadFails = false;
    await env.els.pendingRetry.onclick();
    await env.settle();
    check('R3 retry uploads again', env.server.uploads.length === 2,
          `${env.server.uploads.length} attempts`);
    check('R3 an accepted take is released', env.peek('_pendingBlob') === null);
    check('R3 the recovery row is hidden again',
          env.els.pendingRow.style.display === 'none');
  }
  {
    const env = createEnv(ROOT);
    env.poke('_chunks = [{type: "audio/webm", size: 10}]; _cancelled = true;');
    await env.app.onBrowserStop();
    await env.settle();
    check('a cancelled take is neither uploaded nor retained',
          env.server.uploads.length === 0 && env.peek('_pendingBlob') === null);
  }

  // ----------------------------------------------------------------- G1 ----
  // A retained take is the only copy. Nothing may overwrite it silently.
  {
    const env = createEnv(ROOT);
    env.server.uploadFails = true;
    env.poke('_chunks = [{type: "audio/webm", size: 11}]; _cancelled = false;');
    await env.app.onBrowserStop();
    await env.settle();
    const firstId = env.peek('_pendingId');
    check('G1 take A is retained after its upload fails',
          env.peek('_pendingBlob') !== null);

    // Starting another capture while A is unresolved must be refused.
    await env.app.startBrowserCapture('both');
    await env.settle();
    check('G1 a new capture is refused while a take is unresolved',
          env.peek('browserRec') === false && env.peek('_pendingId') === firstId);
  }
  {
    // An overlapping retry must not let a later result clear an earlier take.
    const env = createEnv(ROOT);
    env.server.uploadFails = true;
    env.poke('_chunks = [{type: "audio/webm", size: 12}]; _cancelled = false;');
    await env.app.onBrowserStop();
    await env.settle();

    let release;
    env.server.uploadGate = new Promise(r => { release = r; });
    env.server.uploadFails = false;
    const retry = env.els.pendingRetry.onclick();
    await env.settle();

    // While that retry is in flight, the pending slot is replaced by a newer
    // take. The in-flight response must not clear it.
    env.poke('_pendingBlob = {size: 99}; _pendingName = "newer.webm"; _pendingId += 1;');
    const newerId = env.peek('_pendingId');
    release();
    await retry;
    await env.settle();
    check('G1 a stale upload result does not clear a newer take',
          env.peek('_pendingBlob') !== null && env.peek('_pendingId') === newerId);
  }

  // ----------------------------------------------------------------- G2 ----
  // One recorder error is one take, not two uploads.
  {
    const env = createEnv(ROOT);
    await env.app.startBrowserCapture('both');
    await env.settle();
    const rec = env.media.recorders[env.media.recorders.length - 1];
    rec.ondataavailable({data: {size: 7, type: 'audio/webm'}});   // some audio
    rec.failWithError();
    await env.settle(20);
    check('G2 a recorder error produces exactly one upload',
          env.server.uploads.length === 1,
          `${env.server.uploads.length} upload(s)`);
    check('G2 the page is not left recording after an error',
          env.peek('browserRec') === false);
  }

  // ----------------------------------------------------------------- G4 ----
  // The error fallback is a timer for a browser that never sends stop. When
  // stop does arrive, that timer has to go with the take it belonged to: it
  // used to survive, find the shared _finalized flag reset by the next
  // capture, and end that recording two seconds in — uploading its opening
  // chunks as a whole take and losing everything said afterwards, because the
  // real stop event then found the handoff already done.
  {
    const env = createEnv(ROOT);
    env.poke('_stopFallbackMs = 40;');   // the shipped 2s, without the wait

    await env.app.startBrowserCapture('both');
    await env.settle();
    const first = env.media.recorders[env.media.recorders.length - 1];
    first.ondataavailable({data: {size: 7, type: 'audio/webm'}});
    first.failWithError();               // error, then the ordinary data + stop
    await env.settle();
    check('G4 the errored take is handed off once', env.server.uploads.length === 1,
          `${env.server.uploads.length} upload(s)`);

    // A second take, started inside the old take's fallback window.
    await env.app.startBrowserCapture('both');
    await env.settle();
    const second = env.media.recorders[env.media.recorders.length - 1];
    check('G4 the next take starts',
          env.peek('browserRec') === true && second.state === 'recording');

    await env.settle(80);                // past when the old timer would fire
    check('G4 a stale fallback timer does not end the next take',
          env.peek('browserRec') === true && env.server.uploads.length === 1,
          `browserRec=${env.peek('browserRec')} uploads=${env.server.uploads.length}`);
    check('G4 the next take still holds its recorder', second.state === 'recording');

    // ...and it still hands off everything when it really does stop.
    second.stop();
    await env.settle();
    check('G4 the next take is uploaded when it stops',
          env.server.uploads.length === 2, `${env.server.uploads.length} upload(s)`);
  }

  // ----------------------------------------------------------------- N5 ----
  // Revoking the share ends the take rather than recording silence.
  {
    const env = createEnv(ROOT);
    await env.app.startBrowserCapture('both');
    await env.settle();
    env.peek('_streams')[0].getAudioTracks()[0].fire('ended');
    await env.settle(20);
    check('N5 a source that ends finishes the take',
          env.peek('browserRec') === false && env.server.uploads.length === 1,
          `${env.server.uploads.length} upload(s)`);
  }

  // ---------------------------------------------------------------- R13 ----
  {
    const env = createEnv(ROOT);
    env.server.state.result_rev = 7; env.server.state.summary_rev = 7;
    env.server.resultFails = true;
    await env.app.poll();
    const afterFail = env.server.loadResultCalls;
    env.server.resultFails = false;
    await env.app.poll();
    const afterOk = env.server.loadResultCalls;
    check('R13 a failed result fetch is retried on the next poll',
          afterOk === afterFail + 1, `${afterFail} then ${afterOk} attempts`);
    await env.app.poll();
    check('R13 nothing is re-fetched once it has succeeded',
          env.server.loadResultCalls === afterOk);
  }

  // ----------------------------------------------------------------- G3 ----
  // Responses slower than the poll interval must still render. The generation
  // guard alone discarded every one of them and never committed a revision.
  {
    const env = createEnv(ROOT);
    env.server.state.result_rev = 3; env.server.state.summary_rev = 3;
    let release;
    env.server.resultGate = new Promise(r => { release = r; });

    const polls = [];
    for (let i = 0; i < 5; i++) polls.push(env.app.poll());
    await env.settle();
    check('G3 overlapping polls issue one result fetch, not five',
          env.server.loadResultCalls === 1,
          `${env.server.loadResultCalls} fetch(es) for 5 ticks`);

    release();
    env.server.resultGate = null;
    await Promise.all(polls);
    await env.settle();
    check('G3 the slow response is rendered and its revision committed',
          env.peek('prevResultRev') === 3,
          `prevResultRev=${env.peek('prevResultRev')}`);
  }

  // ------------------------------------------------- empty transcript ------
  // has_result only says a run finished. A take of silence finishes with a
  // result that has no words in it, and Summarize stayed lit over it: pressing
  // it loaded the whole model to be told there was nothing to summarize.
  {
    const env = createEnv(ROOT);
    env.server.state.has_result = true;
    env.server.state.result_rev = 2;
    env.server.result = {lines: [{text: '  ', ts: '0:00', speaker: null}]};
    await env.app.poll();
    await env.settle();
    check('a transcript of only whitespace leaves Summarize disabled',
          env.els.sumBtn.disabled === true);
    check('the dimmed button says why',
          typeof env.els.sumBtn.title === 'string' && env.els.sumBtn.title.length > 0,
          JSON.stringify(env.els.sumBtn.title));

    env.server.state.result_rev = 3;
    env.server.result = {lines: [{text: 'We ship on Friday.', ts: '0:00', speaker: null}]};
    await env.app.poll();
    await env.settle();
    check('a transcript with words enables Summarize',
          env.els.sumBtn.disabled === false);
    check('an enabled button carries no explanation',
          !env.els.sumBtn.title);

    // Starting a new take clears the result; the button has to go dim again
    // rather than keep the last transcript's answer.
    env.server.state.result_rev = 4;
    env.server.state.has_result = false;
    env.server.result = null;
    await env.app.poll();
    await env.settle();
    check('clearing the result disables Summarize again',
          env.els.sumBtn.disabled === true);
  }

  // ------------------------------------------------ seekable timestamps ----
  // Library timestamps move the playhead, but only where there is a recording
  // to move: a session saved with Save Audio off has a transcript and nothing
  // to play, and a stamp that looks pressable and does nothing is worse than
  // one that never offered.
  {
    const env = createEnv(ROOT);
    const doc = {diarized: false, lines: [
      {speaker: null, text: 'first',  start: 0,    ts: '0:00'},
      {speaker: null, text: 'second', start: 36.1, ts: '0:36'},
      {speaker: null, text: 'third',  start: 74.9, ts: '1:14'},
    ]};
    const el = env.peek("$('libTranscript')");
    const stamps = () => el.children.map(line => line.children[0]);

    env.app.renderTranscript(doc, el, true);
    const live = stamps();
    check('every timestamp is seekable when the audio was saved',
          live.length === 3 && live.every(s => s.dataset.at !== undefined),
          JSON.stringify(live.map(s => s.dataset.at)));
    check('the stamp carries the second it points at',
          Number(live[1].dataset.at) === 36.1, String(live[1].dataset.at));

    env.app.renderTranscript(doc, el, false);
    const dead = stamps();
    check('no timestamp is seekable without saved audio',
          dead.length === 3 && dead.every(s => s.dataset.at === undefined),
          JSON.stringify(dead.map(s => s.dataset.at)));

    // The studio panel passes no flag at all and must stay inert: there is no
    // player on that screen for a stamp to drive.
    const studio = env.peek("$('transcript')");
    env.app.renderTranscript(doc, studio);
    check('the studio transcript is never seekable',
          studio.children.every(line => line.children[0].dataset.at === undefined));
  }

  // ------------------------------------------- a picker holds its own value --
  // Saving reads every control in the panel, so a dropdown that cannot show
  // the value the settings already hold reports "" for it — and the next save
  // writes that "" back, discarding a setting the user never touched. A model
  // name the catalog does not list (a legacy large-v2, or one put there by
  // hand) has to stay selectable.
  {
    const env = createEnv(ROOT);
    const catalog = [
      {id: 'tiny', label: 'Tiny', size: '74 MB', downloaded: false, note: 'n'},
      {id: 'large-v3', label: 'Large v3', size: '2.9 GB', downloaded: false, note: 'n'},
    ];

    env.app.fillCatalog('whisper', catalog, '- none -', 'large-v2');
    const sel = env.els.s_model;
    check('a stored name the catalog does not list stays selectable',
          sel.value === 'large-v2',
          `value=${JSON.stringify(sel.value)} options=` +
              JSON.stringify(sel.options.map(o => o.value)));

    env.app.fillCatalog('whisper', catalog, '- none -', 'tiny');
    check('a listed model is not duplicated',
          env.els.s_model.options.filter(o => o.value === 'tiny').length === 1,
          JSON.stringify(env.els.s_model.options.map(o => o.value)));

    // A refill must not move a choice already on screen — the panel rebuilds
    // this list whenever a download finishes.
    env.app.fillCatalog('whisper', catalog, '- none -', 'large-v3');
    check('a refill leaves the visible choice alone',
          env.els.s_model.value === 'tiny', JSON.stringify(env.els.s_model.value));

    // Fresh panel, nothing stored: the placeholder is the honest state.
    const blank = createEnv(ROOT);
    blank.app.fillCatalog('whisper', catalog, '- none -', '');
    check('nothing stored lands on the placeholder',
          blank.els.s_model.value === '',
          JSON.stringify(blank.els.s_model.value));
  }

  // ----------------------------------------------------------------- G5 ----
  // The dialog says "Replace the transcript shown (second pass)". The handler
  // sent an empty name for every overwrite, and an empty name is how the server
  // is told "the session's original" — so the version the user was looking at
  // came back unchanged and transcript.txt, which nobody had mentioned, was
  // destroyed.
  {
    const env = createEnv(ROOT);
    // A radio group where nothing is checked is the overwrite branch; the "keep
    // both" case sets its own querySelector below. Starting a run re-reads the
    // library, and this harness's library is empty, so the selection is put
    // back before each press rather than carried over.
    const select = (tx, sum) => {
      env.poke('libCurrent = "2026-09-06_10-00-00";');
      env.poke('libItem = {id: "2026-09-06_10-00-00", audio: "audio.wav",' +
               ' transcripts: [{name: ""}, {name: "second pass"}],' +
               ' summaries: [{name: ""}, {name: "shorter"}]};');
      env.poke(`libTx = ${JSON.stringify(tx)}; libSum = ${JSON.stringify(sum)};`);
    };

    select('second pass', 'shorter');
    env.poke('libRunKind = "transcribe";');
    env.els.libRunStart.onclick();
    await env.settle();
    check('G5 overwriting a named transcript aims at that version',
          env.server.libRuns.length === 1 &&
              env.server.libRuns[0].kind === 'transcribe' &&
              env.server.libRuns[0].body.name === 'second pass',
          JSON.stringify(env.server.libRuns.map(r => r.body.name)));

    select('second pass', 'shorter');
    env.poke('libRunKind = "summarize";');
    env.els.libRunStart.onclick();
    await env.settle();
    check('G5 overwriting a named summary aims at that version',
          env.server.libRuns.length === 2 &&
              env.server.libRuns[1].kind === 'summarize' &&
              env.server.libRuns[1].body.name === 'shorter',
          JSON.stringify(env.server.libRuns.map(r => r.body.name)));

    // The original is still reachable: it is what "the version shown" means
    // when the original is the one selected.
    select('', 'shorter');
    env.poke('libRunKind = "transcribe";');
    env.els.libRunStart.onclick();
    await env.settle();
    check('G5 overwriting the original still names the original',
          env.server.libRuns.length === 3 && env.server.libRuns[2].body.name === '',
          JSON.stringify(env.server.libRuns.map(r => r.body.name)));

    // And "keep both" is unaffected: it sends the name that was typed.
    env.els.libRun.querySelector = () => ({value: 'new'});
    env.els.libRunName.value = '  third pass  ';
    select('second pass', 'shorter');
    env.poke('libRunKind = "transcribe";');
    env.els.libRunStart.onclick();
    await env.settle();
    check('G5 keeping both sends the name that was typed',
          env.server.libRuns.length === 4 &&
              env.server.libRuns[3].body.name === 'third pass',
          JSON.stringify(env.server.libRuns.map(r => r.body.name)));
  }

  // -------------------------------------------------- cancelling a run ------
  // Cancel used to be dimmed for the whole of a transcription or a summary, so
  // a run started by mistake had to be waited out — minutes, on a long
  // recording. The engines could always give up mid-run; nothing asked them.
  {
    const env = createEnv(ROOT);
    env.server.state.processing = true;
    env.server.state.phase = 'transcribe';
    await env.app.poll();
    check('Cancel is live while a job runs', env.els.cancelBtn.disabled === false);
    check('and says what it will do', !!env.els.cancelBtn.title,
          JSON.stringify(env.els.cancelBtn.title));

    // Idle with nothing recorded, nothing transcribed: there is nothing for it
    // to stop or throw away.
    env.server.state.processing = false;
    env.server.state.phase = 'idle';
    env.server.state.has_audio = false;
    env.server.state.has_result = false;
    env.server.state.has_summary = false;
    await env.app.poll();
    check('and dim when there is nothing to stop or discard',
          env.els.cancelBtn.disabled === true);
    check('with no explanation to give', !env.els.cancelBtn.title,
          JSON.stringify(env.els.cancelBtn.title));
  }

  console.log(failures ? `\nui: ${failures} check(s) FAILED`
                       : '\nui: all checks passed');
  process.exit(failures ? 1 : 0);
}

run().catch(e => { console.error(e); process.exit(1); });
