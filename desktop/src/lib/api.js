// Requests to the engine. They are addressed to this page's own origin; the
// app's main process forwards /api/* and adds the engine's token on the way
// (electron/main.js), so there is nothing to authorize with from here.

// 502 and 503 are the forwarder saying the engine did not answer. Thrown, so
// that every caller's "the request failed" path covers them: read as a body
// they would look like a state with nothing in it.
export async function api(path, options) {
  const res = await fetch(path, options);
  if (res.status === 502 || res.status === 503) throw new Error('engine unreachable');
  return res.json();
}

export function post(path, body) {
  return api(path, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: body ? JSON.stringify(body) : null,
  });
}

// A file for the pipeline: an upload picked from disk, or a take recorded here.
export function upload(blob, name) {
  const form = new FormData();
  if (name) form.append('file', blob, name); else form.append('file', blob);
  return api('/api/process_file', {method: 'POST', body: form});
}
