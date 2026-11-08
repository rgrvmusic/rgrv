/* SPDX-License-Identifier: MIT */
/*
 * RGRV -- a phrase of letters and numbers, turned into an arpeggio.
 * Copyright (C) 2026 Gerard Braad (Regroove | RGRV).
 *
 * The phrase is the score: A-Z are chromatic keys, 1-0 are the ten notes
 * leading up to A, ',' and '.' are rests, '!' accents and '?' ties the note
 * before it into the one after it.  The parser turns
 * that into notes on a sixteenth-note grid and leaves the output -- a
 * Standard MIDI File, raw MIDI on stdout, or a live RtMidi port -- to
 * midi.c.
 */

#ifndef RGRV_H
#define RGRV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RGRV_VERSION "1.0.0"

/* Ticks to the quarter note.  480 divides by both 16 (sixteenths) and 12
 * (triplets), so a phrase written on the sixteenth grid lines up with what a
 * sequencer expects, and plsynth (which reads the division and divides by it)
 * does not have to round. */
#define RGRV_PPQN 480
#define RGRV_STEP_TICKS (RGRV_PPQN / 4) /* one sixteenth */
#define RGRV_STEPS_PER_BAR 16
#define RGRV_BASE_NOTE 60 /* middle C: the default first letter */
#define RGRV_DRUM_CHANNEL 9 /* MIDI channel 10, counting from one */

/* Channel 10 is the GM drum channel; a RGRV drum part is always written
 * there, and the notes are the two GM keys everything understands. */
#define RGRV_KICK_NOTE 36
#define RGRV_SNARE_NOTE 38

typedef struct rgrv_note {
    uint32_t tick; /* start, absolute in the phrase */
    uint32_t dur;  /* length in ticks */
    uint8_t note;  /* 0..127 */
    uint8_t velocity;
    uint8_t channel; /* 0..15, assigned by the output stage */
    bool accent;
    bool slide; /* '?' extended this note into the next one */
} rgrv_note;

typedef struct rgrv_phrase {
    rgrv_note *notes;
    size_t count;
    size_t cap;
    uint32_t steps;     /* length of the phrase in sixteenth-note steps */
    uint32_t loop_start;/* step inside the first (...) */
    uint32_t loop_end;  /* step of its ')', exclusive */
    bool has_loop;
    bool clamped;       /* at least one note was folded into 0..127 */
} rgrv_phrase;

typedef struct rgrv_parse_opts {
    int octave;      /* octave offset from RGRV_BASE_NOTE, may be negative */
    int transpose;   /* extra semitones, may be negative */
    int gate;        /* note length as a percentage of a step, 1..100 */
    bool chord;      /* each letter becomes a major triad */
    int chord_hold;  /* how many steps a chord is held, >= 1 */
} rgrv_parse_opts;

typedef struct rgrv_drums {
    /* One bar of velocities, 0 = no hit.  The pattern is repeated for every
     * bar of the output. */
    uint8_t kick[RGRV_STEPS_PER_BAR];
    uint8_t snare[RGRV_STEPS_PER_BAR];
} rgrv_drums;

typedef struct rgrv_event {
    uint32_t tick;
    uint8_t msg[3];
    uint8_t len;
    uint8_t rank; /* 0 = note-off, 1 = note-on: offs sort first at one tick */
} rgrv_event;

typedef struct rgrv_song {
    rgrv_event *events;
    size_t count;
    size_t cap;
    uint32_t end_tick;
    uint32_t steps;
} rgrv_song;

/* phrase.c ------------------------------------------------------------- */

/* Parse `text` into notes on the sixteenth grid.  On failure returns false
 * and puts a one-line reason in `err`.  A phrase with no letters is not an
 * error by itself -- a drum-only run is legal -- but `out->steps` is then
 * zero and the caller decides whether that is usable. */
bool rgrv_parse_phrase(const char *text, const rgrv_parse_opts *opts,
                       rgrv_phrase *out, char *err, size_t errlen);
void rgrv_phrase_free(rgrv_phrase *p);

/* Parse a --bassdrum/--snare value: a count 1..16 (spread evenly, snare
 * starting on the backbeat) or an x/o pattern (x hit, o rest, X accent).
 * `snare` selects the default velocity and the numeric offset. */
bool rgrv_drum_pattern(const char *value, bool snare,
                       uint8_t out[RGRV_STEPS_PER_BAR],
                       char *err, size_t errlen);

/* midi.c --------------------------------------------------------------- */

/* Flatten the phrase (repeated `repeat` times), the drum bar and the bar
 * rounding into one tick-sorted event list.  `channel` is the melodic
 * channel, 0..15. */
bool rgrv_build_song(const rgrv_phrase *p, const rgrv_drums *d,
                     int channel, int repeat, rgrv_song *out,
                     char *err, size_t errlen);
void rgrv_song_free(rgrv_song *s);

/* Write a format-0 Standard MIDI File: one track, because plsynth (and
 * nts_smf, the reader behind it) reads exactly one.  `name` is the track
 * name. */
bool rgrv_write_smf(const char *path, const rgrv_song *song,
                    const char *name, int bpm, char *err, size_t errlen);

/* Serialize the same file in memory.  `*out` is malloc'd and belongs to the
 * caller. */
bool rgrv_smf_bytes(const rgrv_song *song, const char *name, int bpm,
                    uint8_t **out, size_t *out_len, char *err, size_t errlen);

/* Live output.  `channel` is 0..15, or -1 to drive every channel (the live
 * default).  `use_rtmidi` selects an RtMidi output port; otherwise the raw
 * MIDI byte stream goes to stdout.  Plays until SIGINT/SIGTERM, or for one
 * pass of the phrase when `once` is true.  Prints the "We have regrooved ..."
 * report itself, once the destination is known.  Returns 0 on a clean stop,
 * 1 on a setup error. */
int rgrv_play(const rgrv_phrase *p, const rgrv_drums *d, int channel,
              int bpm, int gate, int port, bool use_rtmidi, bool once,
              const char *display);

/* List RtMidi output ports and return 0, or explain why there are none. */
int rgrv_list_ports(void);

/* True when the binary was built with RtMidi support. */
bool rgrv_have_rtmidi(void);

#endif /* RGRV_H */
