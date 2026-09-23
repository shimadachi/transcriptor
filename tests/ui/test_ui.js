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
//   V1  Enter on a focused Cancel confirmed the dialog it was declining
//   V6  numbered summary sections all rendered as "1.", their bullets flattened
//   V12 a stopped library re-run was announced as done, and could not be
//       stopped from the library at all
//   V17 "Saved →" and "Not saved →" stood side by side for the same folder
//   V23 a summary cut off at the maximum answer length was shown as finished

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

  // ----------------------------- following the transcript while it plays ----
  // The line being spoken is lit as a saved recording runs, so the panel can be
  // read and listened to at once. It is driven off the player's clock, not off
  // a timer of its own, so scrubbing moves it as well as playing.
  {
    const env = createEnv(ROOT);
    const doc = {diarized: false, lines: [
      {speaker: null, text: 'first',  start: 0,    end: 5,    ts: '0:00'},
      {speaker: null, text: 'second', start: 36.1, end: 40,   ts: '0:36'},
      {speaker: null, text: 'third',  start: 74.9, end: 80.2, ts: '1:14'},
    ]};
    const el = env.peek("$('libTranscript')");
    const audio = env.peek("$('libAudio')");
    audio.setAttribute('src', '/api/library/audio?id=x');
    audio.getAttribute = () => '/api/library/audio?id=x';
    audio.paused = true;               // no chasing the line in these checks
    env.app.renderTranscript(doc, el, true);

    const lit = () => el.children.findIndex(l => l._cls.has('at'));
    const at = (sec) => { audio.currentTime = sec; env.app.plPaint(); return lit(); };

    check('nothing is lit before the recording starts moving', lit() === -1);
    check('the opening line lights at 0:00', at(0) === 0, String(at(0)));
    check('and stays lit through the line', at(3.4) === 0, String(at(3.4)));
    check('a later moment lights the line that covers it',
          at(37) === 1, String(at(37)));
    check('scrubbing backwards moves it back', at(1) === 0, String(at(1)));
    check('the last line lights too', at(75) === 2, String(at(75)));

    // A gap shorter than the grace holds the previous line rather than
    // blinking the highlight off between two turns that butt up together.
    check('a moment just past a line still holds it', at(40.8) === 1,
          String(at(40.8)));
    // A real silence is longer than that, and does go dark.
    check('a real silence clears it', at(60) === -1, String(at(60)));
    check('and past the end it goes dark as well', at(200) === -1,
          String(at(200)));

    // A recording saved without its audio has nothing to follow, so the lines
    // are never indexed and nothing is ever lit.
    env.app.renderTranscript(doc, el, false);
    audio.currentTime = 37;
    env.app.plPaint();
    check('a transcript with no audio is never lit', lit() === -1, String(lit()));

    // The studio panel redraws on every poll. It must not wipe the index built
    // for the recording open in the library tab.
    env.app.renderTranscript(doc, el, true);
    audio.currentTime = 37;
    env.app.plPaint();
    env.app.renderTranscript(doc, env.peek("$('transcript')"));
    env.app.plPaint();
    check('a studio redraw leaves the library highlight alone',
          lit() === 1, String(lit()));

    // Opening a session whose transcript is a plain .txt never reaches
    // renderTranscript. The index has to be dropped anyway, or the highlight
    // keeps chasing lines that are no longer on the page.
    env.poke('libItem = {id: "x", audio: "audio.wav", transcript_text: "no stamps"}');
    env.app.renderLibraryDetail();
    audio.currentTime = 37;
    env.app.plPaint();
    check('a transcript with no timestamps drops the old index',
          env.peek('plLines.length') === 0,
          String(env.peek('plLines.length')));
  }

  // --------------------------------------- following is the reader's call ----
  // Someone working through the text with the audio running behind it does not
  // want the panel moving under them. One press stops it, and the highlight
  // goes with it: a line lit where the panel will not go is half a feature.
  {
    const mkDoc = () => ({diarized: false, lines: [
      {speaker: null, text: 'first',  start: 0,    end: 5,  ts: '0:00'},
      {speaker: null, text: 'second', start: 36.1, end: 40, ts: '0:36'},
    ]});
    const play = (env) => {
      const el = env.peek("$('libTranscript')");
      const audio = env.peek("$('libAudio')");
      audio.getAttribute = () => '/api/library/audio?id=x';
      audio.paused = true;
      env.app.renderTranscript(mkDoc(), el, true);
      return {
        lit: () => el.children.findIndex(l => l._cls.has('at')),
        at: (sec) => { audio.currentTime = sec; env.app.plPaint(); },
      };
    };

    {
      const env = createEnv(ROOT);
      const p = play(env);
      const btn = env.els.plFollowBtn;

      check('following is on when nothing has been said otherwise',
            env.peek('plFollowOn') === true);
      check('and the button does not read as switched off', !btn._cls.has('off'));

      p.at(37);
      check('the line being spoken is lit', p.lit() === 1, String(p.lit()));

      btn.fire('click');
      check('switching following off clears the lit line', p.lit() === -1,
            String(p.lit()));
      check('and the button shows it', btn._cls.has('off'));
      p.at(1);
      check('and no later moment lights one', p.lit() === -1, String(p.lit()));
      check('the choice is remembered', env.storage.getItem('transcriptor-follow') === '0',
            JSON.stringify(env.storage.getItem('transcriptor-follow')));

      btn.fire('click');
      check('switching it back on lights where the recording is now',
            p.lit() === 0, String(p.lit()));
      check('and the button reads as on again', !btn._cls.has('off'));
      check('which is remembered too', env.storage.getItem('transcriptor-follow') === '1',
            JSON.stringify(env.storage.getItem('transcriptor-follow')));
    }

    // A reader who switched it off finds it off next time, and only that
    // stored answer turns it off: anything else is a session that starts on.
    {
      const env = createEnv(ROOT, {storage: {'transcriptor-follow': '0'}});
      const p = play(env);
      p.at(37);
      check('a stored "off" survives the reload', env.peek('plFollowOn') === false);
      check('and nothing is lit on the way in', p.lit() === -1, String(p.lit()));
      check('the button comes up crossed out', env.els.plFollowBtn._cls.has('off'));
    }
    {
      const env = createEnv(ROOT, {storage: {'transcriptor-follow': 'yes please'}});
      const p = play(env);
      p.at(37);
      check('a value nobody wrote leaves following on', p.lit() === 1,
            String(p.lit()));
    }
  }

  // ------------------------------------------ Cancel asks before it acts ----
  // One button gives up three different things, so the question has to name
  // the one in front of it, and declining has to leave the run alone.
  {
    const env = createEnv(ROOT);
    env.server.state.processing = true;
    env.server.state.phase = 'transcribe';
    await env.app.poll();

    const pending = env.els.cancelBtn.onclick();
    await env.settle();
    check('a running job is asked about before it is stopped',
          env.els.askTitle.textContent === 'ask.cancelJobTitle',
          env.els.askTitle.textContent);
    check('and the decline button offers to let it run',
          env.els.askNo.textContent === 'ask.cancelJobNo',
          env.els.askNo.textContent);
    env.els.askNo.onclick();
    await pending;
    check('declining sends nothing at all',
          env.server.cancels.length === 0,
          JSON.stringify(env.server.cancels.length));

    const accepted = env.els.cancelBtn.onclick();
    await env.settle();
    env.els.askYes.onclick();
    await accepted;
    check('accepting stops the run', env.server.cancels.length === 1);
  }

  // A take or a result on screen is a different loss, and says so.
  {
    const env = createEnv(ROOT);
    env.server.state.processing = false;
    env.server.state.phase = 'done';
    env.server.state.has_result = true;
    env.server.cancelReply = {ok: true};
    await env.app.poll();

    const pending = env.els.cancelBtn.onclick();
    await env.settle();
    check('a finished result is asked about as a discard',
          env.els.askTitle.textContent === 'ask.cancelDiscardTitle',
          env.els.askTitle.textContent);
    env.els.askYes.onclick();
    await pending;
    check('and is discarded once agreed', env.server.cancels.length === 1);
  }

  // Nothing running, nothing held, just an error banner: asking there would
  // train the answer out of people, so it goes straight through.
  {
    const env = createEnv(ROOT);
    Object.assign(env.server.state, {
      processing: false, recording: false, phase: 'error',
      has_audio: false, has_result: false, has_summary: false,
    });
    env.server.cancelReply = {ok: true};
    await env.app.poll();

    await env.els.cancelBtn.onclick();
    check('clearing an error asks nothing',
          !env.els.askBg._cls.has('on') && env.server.cancels.length === 1,
          JSON.stringify(env.server.cancels.length));
  }

  // The run can finish while the question is on screen. /api/cancel on a
  // finished run discards its result rather than stopping anything, and
  // agreeing to stop a job is not agreeing to throw the transcript away.
  {
    const env = createEnv(ROOT);
    env.server.state.processing = true;
    env.server.state.phase = 'transcribe';
    await env.app.poll();

    const pending = env.els.cancelBtn.onclick();
    await env.settle();
    env.server.state.processing = false;      // it finished while we deliberated
    env.server.state.phase = 'done';
    env.els.askYes.onclick();
    await pending;
    check('a run that finished while the question was up is left alone',
          env.server.cancels.length === 0,
          JSON.stringify(env.server.cancels.length));
  }

  // ----------------------------------------------------------------- V1 ----
  // The confirm dialog guards deleting a recording and discarding a take.
  // Enter has to press the button that has focus, not always the first one.
  {
    const env = createEnv(ROOT);
    const answer = env.app.ask({title: 'Delete this recording?'});
    env.els.askNo.focus();                    // Tab over to Cancel
    env.fireDoc('keydown', {key: 'Enter', preventDefault() {}});
    check('V1 Enter on a focused Cancel declines', (await answer) === false);
  }
  {
    const env = createEnv(ROOT);
    const answer = env.app.ask({title: 'Delete this recording?'});
    env.fireDoc('keydown', {key: 'Enter', preventDefault() {}});
    check('V1 Enter on the button the dialog opens on still confirms',
          (await answer) === true);
  }

  // ----------------------------------------------------------------- V6 ----
  // The shape summaries come back in: numbered sections, bullets under each.
  {
    const env = createEnv(ROOT);
    const html = env.app.mdToHtml(
      '1. **Roadmap**\n   - Beta ships Friday\n   - Notes by Ayşe\n' +
      '2. **Risks**\n   - QA is thin\n3. **Next steps**');
    check('V6 numbered sections with bullets under them stay one list',
          (html.match(/<ol/g) || []).length === 1, html);
    check('V6 the bullets nest inside their section',
          html.includes('<li><strong>Roadmap</strong><ul><li>Beta ships Friday</li>'),
          html);
    const loose = env.app.mdToHtml('1. First\n\n2. Second');
    check('V6 a blank line between items does not start the count again',
          (loose.match(/<ol/g) || []).length === 1, loose);
    const broken = env.app.mdToHtml('1. First\n\nA paragraph.\n\n2. Second');
    check('V6 a list a paragraph broke into carries on counting',
          broken.includes('<ol start="2">'), broken);
    const flat = env.app.mdToHtml('- a\n- b\n\nAfter the list.');
    check('V6 a plain list still ends where the text after it begins',
          flat === '<ul><li>a</li><li>b</li></ul><p>After the list.</p>', flat);
  }

  // ---------------------------------------------------------------- V12 ----
  // A stopped job ends on "idle", so the error phase alone cannot tell a stop
  // from a finish; job_cancelled can.
  const itemAsked = env =>
    env.server.requests.filter(u => u.startsWith('/api/library/item')).pop() || '';
  {
    const env = createEnv(ROOT);
    env.server.library = {sessions: [{id: 'S1'}]};
    env.poke('libCurrent = "S1"; libTx = ""; libSum = "";');
    await env.app.libRunEnded('summarize', 'second pass',
      {processing: false, phase: 'idle', job_cancelled: true, message: 'Stopped.'});
    check('V12 a stopped re-run is not announced as done',
          env.els.toast.textContent === 'lib.runStopped', env.els.toast.textContent);
    check('V12 and the version it never wrote is not asked for',
          !itemAsked(env).includes('second%20pass'), itemAsked(env));
  }
  {
    const env = createEnv(ROOT);
    env.server.library = {sessions: [{id: 'S1'}]};
    env.poke('libCurrent = "S1"; libTx = ""; libSum = "";');
    await env.app.libRunEnded('summarize', 'second pass',
      {processing: false, phase: 'done', job_cancelled: false, message: 'Done'});
    check('a finished re-run is announced and its new version opened',
          env.els.toast.textContent === 'lib.runDone' &&
            itemAsked(env).includes('summary=second%20pass'),
          env.els.toast.textContent + ' ' + itemAsked(env));
  }
  {
    // Stopping from the library stops the job and nothing else: if the run
    // has already finished, the studio's take must not be discarded instead.
    const env = createEnv(ROOT);
    env.server.state.processing = true;
    env.server.state.phase = 'summarizing';
    await env.app.poll();
    check('V12 the library shows a way to stop the run', env.els.libStop.hidden === false);
    const pending = env.els.libStop.onclick();
    await env.settle();
    env.els.askYes.onclick();
    await pending;
    check('V12 and asks the server to stop the job alone',
          env.server.cancels.length === 1 && env.server.cancels[0].body.job_only === true,
          JSON.stringify(env.server.cancels.map(c => c.body)));
  }

  // ---------------------------------------------------------------- V17 ----
  {
    const env = createEnv(ROOT);
    Object.assign(env.server.state, {output_dir: '/out/2026-09-23_10-00-00',
      save_error: 'The audio could not be written to /out/2026-09-23_10-00-00'});
    await env.app.poll();
    const label = () => (env.els.savedK || {}).textContent;
    check('V17 a folder whose take failed to write is not called saved',
          label() === 'saved.folder' && env.els.saveErr.style.display === 'flex',
          String(label()));
    env.server.state.save_error = null;
    await env.app.poll();
    check('a take that was written is still "Saved"',
          label() === 'saved.k', String(label()));
  }

  // ---------------------------------------------------------------- V23 ----
  {
    const env = createEnv(ROOT);
    env.server.summary = '## Summary\n- The beta ships on';
    env.server.summaryCutShort = true;
    await env.app.loadResult();
    const note = () => env.els.sumCut || {};
    check('V23 a summary cut at the answer limit carries a note saying so',
          note().hidden === false);
    env.server.summaryCutShort = false;
    await env.app.loadResult();
    check('a finished summary carries no note', note().hidden !== false);
  }
  {
    const env = createEnv(ROOT);
    env.server.library = {sessions: [{id: 'S1'}]};
    env.poke('libCurrent = "S1"; libTx = ""; libSum = "";');
    const msg = 'Done, but the summary reached the maximum answer length (256 tokens)';
    await env.app.libRunEnded('summarize', 'short',
      {processing: false, phase: 'done', job_cancelled: false, summary_cut_short: true,
       message: msg});
    check('V23 a library summary cut at the limit is announced as cut',
          env.els.toast.textContent === msg, env.els.toast.textContent);
    env.app.renderLibStatus({processing: false, phase: 'done', message: msg});
    const readout = () => (env.els.libStatusMsg || {}).textContent;
    check('V23 and the readout keeps saying so after the toast',
          env.els.libStatus.hidden === false && env.els.libStatus._cls.has('warn') &&
            readout() === msg,
          String(readout()));
    check('V23 while the new version is still opened',
          itemAsked(env).includes('summary=short'), itemAsked(env));
  }

  console.log(failures ? `\nui: ${failures} check(s) FAILED`
                       : '\nui: all checks passed');
  process.exit(failures ? 1 : 0);
}

run().catch(e => { console.error(e); process.exit(1); });
