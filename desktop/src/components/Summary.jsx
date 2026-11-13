import {useMemo} from 'react';

import {mdToHtml} from '../lib/markdown.js';
import {openLink} from '../state/studio.js';

// A summary, rendered from the model's Markdown. The one place HTML is set
// directly: mdToHtml() escapes its input before it builds anything, so what
// the model wrote can only ever come out as text inside the tags made here.
export function Summary({id, text, emptyText}) {
  const html = useMemo(() => (text ? mdToHtml(text) : ''), [text]);

  // A link opens in the real browser. `href`, the property, is the URL as the
  // page parsed it: absolute and plain ASCII, which is what the engine accepts.
  const onClick = e => {
    const a = e.target && e.target.closest && e.target.closest('a[href]');
    if (!a) return;
    e.preventDefault();
    openLink(a.href);
  };

  if (!text) {
    return (
      <div id={id} className="sum-body">
        <span className="empty">{emptyText || '—'}</span>
      </div>
    );
  }
  return <div id={id} className="sum-body filled" onClick={onClick}
              dangerouslySetInnerHTML={{__html: html}}></div>;
}
