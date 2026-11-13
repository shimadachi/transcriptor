// The toast and the app's own confirm. Callable from anywhere -- a timer, a
// recorder callback -- which is why they are functions over the store rather
// than something a component owns.
import {app} from '../lib/store.js';

let toastTimer = null;
export function toast(msg) {
  app.set({toast: {msg, on: true}});
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => app.set({toast: {msg, on: false}}), 2400);
}

// The question is asked in the app's own dialog, in its language and colours,
// not the system's. Resolves to whether the answer was yes.
let askResolve = null;
export function ask(opts) {
  return new Promise(resolve => {
    askClose(false);            // never leave one hanging behind another
    askResolve = resolve;
    app.set({ask: {title: opts.title || '', body: opts.body || '',
                   yes: opts.yes || '', no: opts.no || ''}});
  });
}
export function askClose(answer) {
  if (!askResolve) return;
  const resolve = askResolve;
  askResolve = null;
  app.set({ask: null});
  resolve(answer);
}
export const asking = () => !!askResolve;
