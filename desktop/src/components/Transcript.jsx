import {t} from '../i18n.js';
import {SPEAKER_COLORS} from '../lib/tables.js';

// A finished transcript's lines: timestamp, speaker, text.
//
// `seekable` turns the timestamps into controls that move the playback head.
// Only the library passes it, and only when the recording kept its audio:
// there is nothing to jump into otherwise, and a timestamp that looks
// pressable and does nothing is worse than one that never offered.
//
// `lineRef(i, el)` hands each line's element back, for the library to light
// the one being spoken.
export function TranscriptLines({res, seekable, lineRef}) {
  const speakers = {};
  return res.lines.map((l, i) => {
    const spoken = res.diarized && l.speaker !== null && l.speaker !== undefined;
    if (spoken && !(l.speaker in speakers)) speakers[l.speaker] = Object.keys(speakers).length;
    const at = seekable && typeof l.start === 'number' ? l.start : null;
    return (
      <div key={i} ref={lineRef ? el => lineRef(i, el) : undefined}
           className={'line' + (spoken ? ' spk' : '')}
           style={spoken ? {'--sc': 'var(' +
             SPEAKER_COLORS[speakers[l.speaker] % SPEAKER_COLORS.length] + ')'} : undefined}>
        {l.ts && (at === null
          ? <span className="ts">{l.ts}</span>
          // data-at is what the click handler and the stylesheet both key off,
          // so the two can never disagree about which stamps are live.
          : <span className="ts" data-at={at} role="button" tabIndex={0}
                  title={t('lib.seekTo')}>{l.ts}</span>)}
        {spoken && <span className="spk-tag">{l.speaker_name}</span>}
        <span className="txt">{l.text}</span>
      </div>
    );
  });
}

export const hasLines = res => !!(res && res.lines && res.lines.length);
