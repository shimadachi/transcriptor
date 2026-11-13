// ---- minimal, offline Markdown -> HTML (for the summary) ----
// Input is HTML-escaped first, so model output can never inject markup. That
// is what makes it safe for the one place the interface sets HTML directly
// (components/Summary.jsx); everything else goes through React as text.

export function esc(s){ return String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }

function mdEmphasis(s){
  return s
    .replace(/`([^`]+)`/g, '<code>$1</code>')
    .replace(/~~([^~]+)~~/g, '<del>$1</del>')
    .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
    .replace(/__([^_]+)__/g, '<strong>$1</strong>')
    .replace(/(^|[^*])\*([^*\n]+)\*/g, '$1<em>$2</em>')
    .replace(/(^|[^_])_([^_\n]+)_/g, '$1<em>$2</em>');
}
function mdInline(s){
  // Links come out first and go back in last. A URL is full of the characters
  // the emphasis rules look for -- underscores especially -- so running those
  // over an href would quietly corrupt it.
  const links = [];
  s = s.replace(/\[([^\]\n]*)\]\(([^)\s]+)\)/g, (whole, text, url) => {
    // Model output is untrusted: only ever emit schemes that just navigate,
    // never javascript: or data:.
    if (!/^(https?:\/\/|mailto:)/i.test(url)) return whole;
    return '\u0000L' + (links.push({text, url}) - 1) + '\u0000';
  });
  return mdEmphasis(s).replace(/\u0000L(\d+)\u0000/g, (_, i) => {
    const l = links[+i];
    return `<a href="${l.url}" target="_blank" rel="noopener noreferrer">`
         + `${mdEmphasis(l.text)}</a>`;
  });
}
// A row is a GFM table separator like `| --- | :--: |` (dashes, optional colons).
function isTableSep(line){
  return /^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)+\|?\s*$/.test(line);
}
// Split a `| a | b |` row into trimmed cells, tolerating missing outer pipes.
function splitRow(line){
  let s = line.trim();
  if (s.startsWith('|')) s = s.slice(1);
  if (s.endsWith('|')) s = s.slice(0, -1);
  return s.split('|').map(c => c.trim());
}
function mdTable(header, sep, rows){
  const aligns = splitRow(sep).map(c => {
    const l = c.startsWith(':'), r = c.endsWith(':');
    return l && r ? 'center' : r ? 'right' : l ? 'left' : '';
  });
  const cell = (tag, txt, i) => {
    const a = aligns[i] ? ` style="text-align:${aligns[i]}"` : '';
    return `<${tag}${a}>${mdInline(esc(txt))}</${tag}>`;
  };
  const cols = header.length;
  const pad = (r) => { const c = r.slice(0, cols);
    while (c.length < cols) c.push(''); return c; };
  let h = '<table><thead><tr>';
  pad(header).forEach((c, i) => { h += cell('th', c, i); });
  h += '</tr></thead><tbody>';
  for (const r of rows){ h += '<tr>';
    pad(r).forEach((c, i) => { h += cell('td', c, i); }); h += '</tr>'; }
  return h + '</tbody></table>';
}
// A list item, or null: how deep it sits, which kind of list it belongs to,
// the number the model gave it, and its text. Takes the line already escaped,
// which leaves the markers and the indentation as they were.
function mdListItem(e){
  const m = /^(\s*)(?:([-*•–])|(\d+)[.)])\s+(.*)$/.exec(e);
  if (!m) return null;
  return {indent: m[1].replace(/\t/g, '    ').length, tag: m[2] ? 'ul' : 'ol',
          num: m[3] ? parseInt(m[3], 10) : 1, text: m[4]};
}
export function mdToHtml(text){
  const lines = String(text).replace(/\r\n?/g, '\n').split('\n');
  let html = '', quote = [];
  // Open lists, outermost first, each with its last <li> still open so that a
  // deeper list can go inside it. A numbered section with its bullets indented
  // under it is the shape models write most, and a flat renderer closed the
  // <ol> at the first indented bullet: every section restarted at "1." and its
  // bullets fell out of it.
  const lists = [];
  const closeTo = (indent) => {
    while (lists.length && lists[lists.length - 1].indent > indent) {
      html += `</li></${lists.pop().tag}>`;
    }
  };
  const closeList = () => closeTo(-1);
  const flushQuote = () => { if (quote.length){
    html += `<blockquote>${mdToHtml(quote.join('\n'))}</blockquote>`; quote = []; } };
  for (let i = 0; i < lines.length; i++){
    const line = lines[i].replace(/\s+$/, '');
    // Fenced code block. Checked before everything else: its contents are
    // literal, and a fence full of pipes would otherwise read as a table.
    let fence = /^\s*```+\s*[A-Za-z0-9_+-]*\s*$/.exec(line);
    if (fence){
      closeList(); flushQuote();
      const body = [];
      let j = i + 1;
      for (; j < lines.length && !/^\s*```+\s*$/.test(lines[j]); j++) body.push(lines[j]);
      html += `<pre><code>${esc(body.join('\n'))}</code></pre>`;
      i = j;   // skip the closing fence, or land past the end if unterminated
      continue;
    }
    // GFM pipe table: a header row followed by a separator row.
    if (line.includes('|') && i + 1 < lines.length && isTableSep(lines[i + 1])
        && !isTableSep(line)){
      closeList(); flushQuote();
      const header = splitRow(line); const sep = lines[i + 1];
      const rows = []; let j = i + 2;
      for (; j < lines.length; j++){
        const r = lines[j].replace(/\s+$/, '');
        if (!r.trim() || !r.includes('|')) break;
        rows.push(splitRow(r));
      }
      html += mdTable(header, sep, rows); i = j - 1; continue;
    }
    if (!line.trim()){
      // A blank line between items is a loose list, not the end of one: keep
      // it open when what follows still belongs to it.
      let j = i + 1;
      while (j < lines.length && !lines[j].trim()) j++;
      const next = j < lines.length ? lines[j] : '';
      if (!(lists.length && (mdListItem(esc(next)) || /^\s+\S/.test(next)))) closeList();
      flushQuote(); continue;
    }
    // Blockquote: buffer consecutive `>` lines, render recursively.
    let m = /^\s*>\s?(.*)$/.exec(line);
    if (m){ closeList(); quote.push(m[1]); continue; }
    flushQuote();
    // Horizontal rule.
    if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)){ closeList(); html += '<hr>'; continue; }
    const e = esc(line);
    m = /^(#{1,4})\s+(.*)$/.exec(e);
    if (m){ closeList(); const lvl = Math.min(m[1].length + 2, 6);
      html += `<h${lvl}>${mdInline(m[2])}</h${lvl}>`; continue; }
    const item = mdListItem(e);
    if (item){
      closeTo(item.indent);
      const top = lists[lists.length - 1];
      if (top && top.indent === item.indent && top.tag !== item.tag){
        html += `</li></${lists.pop().tag}>`;    // same depth, other kind of list
      }
      const cur = lists[lists.length - 1];
      if (cur && cur.indent === item.indent){
        html += '</li>';
      } else {
        // `start` keeps the model's own number, so a list a paragraph broke
        // into carries on counting instead of beginning again at 1.
        const start = item.tag === 'ol' && item.num !== 1 ? ` start="${item.num}"` : '';
        html += `<${item.tag}${start}>`;
        lists.push({tag: item.tag, indent: item.indent});
      }
      // GFM task list. The models reach for these constantly for action items,
      // and without this the box renders as a literal "[ ]" in front of the text.
      const task = /^\[([ xX])\]\s+(.*)$/.exec(item.text);
      html += task ? `<li class="task${task[1] === ' ' ? '' : ' done'}">${mdInline(task[2])}`
                   : `<li>${mdInline(item.text)}`;
      continue;
    }
    // Text indented under an item belongs to it -- the explanation models put
    // beneath a numbered heading -- rather than ending the list.
    if (lists.length && line.match(/^\s*/)[0].replace(/\t/g, '    ').length >
                        lists[lists.length - 1].indent){
      html += `<br>${mdInline(e.trim())}`;
      continue;
    }
    closeList();
    html += `<p>${mdInline(e)}</p>`;
  }
  closeList(); flushQuote();
  return html;
}
