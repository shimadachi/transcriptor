// The themed dropdown that stands in for the native <select>, whose popup is
// drawn by the OS: light-on-dark and out of place next to everything else.
//
// The native element is still rendered, hidden by the stylesheet. It carries
// the id and class the stylesheet's rules key off (`.xsel:has(#micSource)`,
// `.xsel:has(.lib-pick)`), so the markup those rules were written against is
// the markup that is here.
import {useEffect, useRef, useState} from 'react';

const ICON = {
  out: (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M3 9v6h4l5 4V5L7 9H3z"/><path d="M16.5 8.5a5 5 0 0 1 0 7"/>
    </svg>
  ),
  mic: (
    <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5 11a7 7 0 0 0 14 0"/>
      <line x1="12" y1="18" x2="12" y2="21"/>
    </svg>
  ),
};

// One list open at a time, and any click or Escape elsewhere closes it.
let closeOpen = null;
function closeAll() { if (closeOpen) { closeOpen(); closeOpen = null; } }
if (typeof document !== 'undefined') {
  document.addEventListener('click', closeAll);
  document.addEventListener('keydown', e => { if (e.key === 'Escape') closeAll(); });
}

// `options` is [{value, label, icon?: 'out' | 'mic'}]. `labelled` says a
// <label id={id + '_lbl'} htmlFor={id + '_btn'}> names this control, so a
// click on the label opens the list and a screen reader hears "Interface
// language, English".
export function Select({id, value, onChange, options, grow, disabled, hidden,
                        className, title, labelled}) {
  const [open, setOpen] = useState(false);
  const cur = options.find(o => o.value === value);
  const btnId = id ? id + '_btn' : undefined;

  // This list's own way of being closed, so "the open one" can be told apart
  // from every other dropdown on the page.
  const closer = useRef(null);
  if (!closer.current) closer.current = () => setOpen(false);
  const release = () => { if (closeOpen === closer.current) closeOpen = null; };

  useEffect(() => { if (disabled && open) { setOpen(false); release(); } }, [disabled, open]);
  // Gone from the screen while open: nothing must be left to close later.
  useEffect(() => release, []);

  const toggle = e => {
    e.stopPropagation();
    if (disabled) return;
    const wasOpen = open;
    closeAll();
    if (!wasOpen) { setOpen(true); closeOpen = closer.current; }
  };
  const pick = (e, v) => {
    e.stopPropagation();
    setOpen(false); release();
    if (v !== value) onChange(v);
  };
  const icon = o => o && o.icon
    ? <span className={'xsel-ic ' + o.icon}>{ICON[o.icon]}</span> : null;

  return (
    <div className={'xsel' + (grow ? ' grow' : '') + (open ? ' open' : '') +
                    (disabled ? ' disabled' : '')} hidden={hidden}>
      <select id={id} className={className} value={value} disabled={disabled}
              tabIndex={-1} aria-hidden="true" onChange={e => onChange(e.target.value)}>
        {options.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
      </select>
      <button type="button" className="xsel-btn" id={btnId} title={title}
              aria-haspopup="listbox" aria-expanded={open}
              aria-labelledby={labelled && id ? id + '_lbl ' + btnId : undefined}
              onClick={toggle}>
        {icon(cur)}
        {/* Every option is laid into the same grid cell as the value, so the
            trigger is as wide as the longest name rather than the chosen one.
            Sized by the selection it shrank when a short one was picked, and
            the popup -- which is the width of the trigger -- then ellipsised
            the very names it is there to show. */}
        <span className="xsel-value">
          <span className="xsel-label">{cur ? cur.label : ''}</span>
          {options.map(o => (
            <span key={o.value} className="xsel-ghost" aria-hidden="true">{o.label}</span>
          ))}
        </span>
        <span className="xsel-caret"></span>
      </button>
      <div className="xsel-pop" role="listbox">
        {options.map(o => (
          <div key={o.value} role="option" aria-selected={o.value === value}
               className={'xsel-opt' + (o.value === value ? ' sel' : '')}
               onClick={e => pick(e, o.value)}>
            {icon(o)}<span className="xsel-label">{o.label}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
