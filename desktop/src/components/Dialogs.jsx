import {useEffect, useRef} from 'react';

import {t} from '../i18n.js';
import {useApp} from '../lib/store.js';
import {askClose} from '../state/ui.js';

export function Toast() {
  const {toast} = useApp();
  return <div id="toast" className={'toast' + (toast.on ? ' on' : '')}>{toast.msg}</div>;
}

// The app's own confirm: the same question a system dialog would ask, in the
// app's language and colours.
export function AskDialog() {
  const {ask} = useApp();
  const yes = useRef(null), no = useRef(null);

  useEffect(() => { if (ask && yes.current) yes.current.focus(); }, [ask]);

  useEffect(() => {
    if (!ask) return undefined;
    const onKey = e => {
      const f = document.activeElement;
      if (e.key === 'Escape') { e.preventDefault(); askClose(false); }
      else if (e.key === 'Enter') {
        // Enter answers with the button that has focus. Treating every Enter
        // as "yes" turned Enter on a focused Cancel into the deletion it was
        // declining. Anywhere else the key is swallowed: nothing behind the
        // dialog may be pressed while it is up.
        e.preventDefault();
        if (f === yes.current) askClose(true);
        else if (f === no.current) askClose(false);
      } else if (e.key === 'Tab') {
        // Focus goes round the two answers and nowhere else. Tab from the last
        // one walked out into the page behind, where Space then pressed
        // whatever it reached. With two buttons, either direction is the other.
        e.preventDefault();
        (f === yes.current ? no.current : yes.current).focus();
      } else if (e.key === ' ' && f !== yes.current && f !== no.current) {
        e.preventDefault();   // the same rule as Enter, for the other key that presses
      }
    };
    // Capture, so this runs before anything behind the dialog hears the key.
    document.addEventListener('keydown', onKey, true);
    return () => document.removeEventListener('keydown', onKey, true);
  }, [ask]);

  // Clicking the backdrop is a decline, the same as Cancel -- and the same as
  // the settings dialog, so the two do not disagree about what a stray click
  // outside them means.
  return (
    <div className={'modal-bg' + (ask ? ' on' : '')} id="askBg"
         onClick={e => { if (e.target === e.currentTarget) askClose(false); }}>
      <div className="modal ask" role="alertdialog" aria-modal="true" aria-labelledby="askTitle">
        <h2 id="askTitle">{ask ? ask.title : ''}</h2>
        <div id="askBody" className="ask-body">{ask ? ask.body : ''}</div>
        <div className="modal-actions">
          <button ref={no} className="ghost" type="button" onClick={() => askClose(false)}>
            {(ask && ask.no) || t('set.cancel')}</button>
          <button ref={yes} className="save" type="button" onClick={() => askClose(true)}>
            {(ask && ask.yes) || t('ask.yes')}</button>
        </div>
      </div>
    </div>
  );
}
