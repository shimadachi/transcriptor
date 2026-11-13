// Constants that are not this file's to decide: each one has a second copy
// somewhere else, named beside it, and the two must agree.

// The clustering threshold is derived from the transcription language by
// Settings::cluster_threshold in src/config.cpp. The settings panel says which
// value is in force, and follows the language select live rather than waiting
// for a save -- so it needs the table before the engine has been told.
export const CLTHR_BY_LANG = {tr: 0.80, en: 0.65, '': 0.70};

// Cancel is three actions wearing one button, and each gives up something
// different. A single "are you sure?" would be worth nothing here, so the
// question names what this particular press is about to throw away.
export const CANCEL_ASK = {
  job:     {title: 'ask.cancelJobTitle',     body: 'ask.cancelJob',
            yes:   'ask.cancelJobYes',       no:   'ask.cancelJobNo'},
  rec:     {title: 'ask.discardTitle',       body: 'ask.cancelRec',
            yes:   'ask.discard',            no:   'ask.cancelRecNo'},
  discard: {title: 'ask.cancelDiscardTitle', body: 'ask.cancelDiscard',
            yes:   'ask.discard',            no:   'ask.cancelDiscardNo'},
};

// Sources recorded by this window rather than by the engine. The engine's own
// list is preferred wherever it has one; these work on every platform.
export const BROWSER_SOURCES = [
  {id: 'browser:both',   key: 'src.browserBoth'},
  {id: 'browser:system', key: 'src.browserSys'},
  {id: 'browser:mic',    key: 'src.browserMic'},
];

// Speaker colours, as the stylesheet names them.
export const SPEAKER_COLORS = ['--s0', '--s1', '--s2', '--s3', '--s4', '--s5', '--s6', '--s7'];

export const METER_BARS = 40;
export const PL_RATES = [1, 1.25, 1.5, 2];
// How many ticks the player's ladder is drawn with. Fixed rather than derived
// from the width: they are spread by the flexbox, so the count only sets how
// fine the scale reads.
export const PL_BARS = 80;
// Seconds past a line's end before the highlight goes dark. Turns usually butt
// up against each other, and blinking the highlight off in the fraction of a
// second between them would be worse than holding it; a real silence is longer
// than this and does clear it.
export const PL_GAP_GRACE = 1.5;
