// Regression tests for the browser-side recording path, run against the real
// web/app.js. Every case here corresponds to a bug that shipped:
//
//   R3  a failed upload dropped the only copy of the take
//   R10 a capture that failed after the permission prompt left the mic open
//   R13 one failed /api/result left the panels stale for ever
//   G1  a second take, or an overlapping retry, destroyed a retained take
//   G2  a recorder error uploaded one take as two
//   G3  slow /api/result responses starved every render

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

  console.log(failures ? `\nui: ${failures} check(s) FAILED`
                       : '\nui: all checks passed');
  process.exit(failures ? 1 : 0);
}

run().catch(e => { console.error(e); process.exit(1); });
