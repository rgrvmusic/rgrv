/* SPDX-License-Identifier: MIT */
/*
 * phrase.c -- turns the phrase into notes on a sixteenth-note grid.
 *
 *   A-Z        chromatic keys from the base note upward
 *   1-0        the ten notes leading into A; 0 is just below it
 *   ,          one sixteenth of rest
 *   .          two sixteenths of rest
 *   !          accent the next note
 *   ?          tie the previous note into the next one
 *   ( ... )    bracket live output loops after the first pass
 *
 * Unrecognised characters take no time. With --chord each note is the root
 * of a major triad, and --hold sets the steps between roots.
 */

#include "rgrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool push_note(rgrv_phrase *p, uint32_t tick, uint32_t dur,
                      uint8_t note, uint8_t velocity, bool accent, bool slide)
{
    if (p->count == p->cap) {
        const size_t cap = p->cap ? p->cap * 2 : 64;
        rgrv_note *grown = (rgrv_note *)realloc(p->notes, cap * sizeof(*grown));
        if (!grown) return false;
        p->notes = grown;
        p->cap = cap;
    }
    rgrv_note *n = &p->notes[p->count++];
    n->tick = tick;
    n->dur = dur;
    n->note = note;
    n->velocity = velocity;
    n->channel = 0;
    n->accent = accent;
    n->slide = slide;
    return true;
}

static int ascii_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

bool rgrv_parse_phrase(const char *text, const rgrv_parse_opts *opts,
                       rgrv_phrase *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (err && errlen) err[0] = '\0';
    if (!text || !opts) {
        if (err) snprintf(err, errlen, "no phrase");
        return false;
    }

    const int base =
        RGRV_BASE_NOTE + opts->octave * 12 + opts->transpose;
    const uint32_t gate_ticks =
        (uint32_t)(((long)RGRV_STEP_TICKS * opts->gate) / 100);
    const uint32_t note_ticks = gate_ticks ? gate_ticks : 1;
    const uint32_t chord_ticks =
        (uint32_t)opts->chord_hold * RGRV_STEP_TICKS;

    uint32_t step = 0;
    bool accent = false;
    bool slide_pending = false;
    bool saw_loop = false;
    bool have_loop_end = false;
    uint32_t loop_start = 0, loop_end = 0;
    int paren_depth = 0;
    size_t last_group_start = 0;
    size_t last_group_count = 0;
    uint32_t last_group_tick = 0;

    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        const int ch = *c;

        int semis = 0;
        bool is_note = false;

        if (ch >= '0' && ch <= '9') {
            /* 1..9,0 walks up chromatically into A. */
            const int value = ch == '0' ? 10 : ch - '0';
            semis = base + value - 11;
            is_note = true;
        } else {
            const int up = ascii_upper(ch);
            if (up >= 'A' && up <= 'Z') {
                semis = base + (up - 'A');
                is_note = true;
            }
        }

        if (is_note) {
            const uint32_t start = step * RGRV_STEP_TICKS;
            const uint32_t dur = opts->chord ? chord_ticks : note_ticks;
            const size_t group_start = out->count;

            /* '?' extends the previous note or chord to this one. */
            if (slide_pending && last_group_count > 0 &&
                start > last_group_tick) {
                for (size_t i = 0; i < last_group_count; i++) {
                    rgrv_note *n = &out->notes[last_group_start + i];
                    n->dur = start - last_group_tick;
                    n->slide = true;
                }
            }

            /* Major triad on the root: root, major third, perfect fifth. */
            static const int chord_intervals[] = {0, 4, 7};
            const int tones = opts->chord ? 3 : 1;
            for (int t = 0; t < tones; t++) {
                int note = semis + (opts->chord ? chord_intervals[t] : 0);
                if (note < 0) {
                    note = 0;
                    out->clamped = true;
                } else if (note > 127) {
                    note = 127;
                    out->clamped = true;
                }
                if (!push_note(out, start, dur, (uint8_t)note,
                               accent ? 127 : 100, accent, false)) {
                    if (err) snprintf(err, errlen, "out of memory");
                    rgrv_phrase_free(out);
                    return false;
                }
            }

            last_group_start = group_start;
            last_group_count = out->count - group_start;
            last_group_tick = start;

            step += opts->chord ? (uint32_t)opts->chord_hold : 1;
            accent = false;
            slide_pending = false;
            continue;
        }

        switch (ch) {
        case '!':
            accent = true;
            break;
        case '?':
            slide_pending = true;
            break;
        case ',':
            step += 1;
            break;
        case '.':
            step += 2;
            break;
        case '(':
            if (paren_depth == 0) {
                saw_loop = true;
                loop_start = step;
            }
            paren_depth++;
            break;
        case ')':
            if (paren_depth > 0) paren_depth--;
            if (saw_loop && paren_depth == 0 && !have_loop_end) {
                loop_end = step;
                have_loop_end = true;
            }
            break;
        default:
            /* Ignored; '!' and '?' still apply to the next note. */
            break;
        }
    }

    out->steps = step;
    if (saw_loop) {
        if (!have_loop_end) loop_end = step;
        if (loop_end > loop_start) {
            out->has_loop = true;
            out->loop_start = loop_start;
            out->loop_end = loop_end;
        }
    }
    return true;
}

void rgrv_phrase_free(rgrv_phrase *p)
{
    if (!p) return;
    free(p->notes);
    memset(p, 0, sizeof(*p));
}

static bool digits_only(const char *s)
{
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

bool rgrv_drum_pattern(const char *value, bool snare,
                       uint8_t out[RGRV_STEPS_PER_BAR],
                       char *err, size_t errlen)
{
    memset(out, 0, RGRV_STEPS_PER_BAR);
    if (err && errlen) err[0] = '\0';
    if (!value || !*value) {
        if (err) snprintf(err, errlen, "empty drum pattern");
        return false;
    }

    const uint8_t normal = snare ? (uint8_t)105 : (uint8_t)110;

    if (digits_only(value)) {
        /* Two digits cover 1..16. */
        if (strlen(value) > 2) {
            if (err) snprintf(err, errlen, "drum count must be 1..%d",
                              RGRV_STEPS_PER_BAR);
            return false;
        }
        const int count = atoi(value);
        if (count < 1 || count > RGRV_STEPS_PER_BAR) {
            if (err)
                snprintf(err, errlen, "drum count must be 1..%d",
                         RGRV_STEPS_PER_BAR);
            return false;
        }
        /* Snare spreads from the second beat, so a count of 2 is the
         * backbeat. */
        const int offset = snare ? 4 : 0;
        for (int i = 0; i < count; i++) {
            const int at = (offset + (i * RGRV_STEPS_PER_BAR) / count) %
                           RGRV_STEPS_PER_BAR;
            out[at] = normal;
        }
        return true;
    }

    const size_t len = strlen(value);
    if (len > 256) {
        if (err) snprintf(err, errlen, "drum pattern too long");
        return false;
    }
    for (int i = 0; i < RGRV_STEPS_PER_BAR; i++) {
        const char c = value[(size_t)i % len];
        switch (c) {
        case 'x':
        case '1':
            out[i] = normal;
            break;
        case 'X':
            out[i] = 127;
            break;
        case 'o':
        case 'O':
        case '0':
        case '.':
        case '-':
        case '_':
            out[i] = 0;
            break;
        default:
            if (err)
                snprintf(err, errlen,
                         "invalid drum step '%c' (use x for hit, o for rest)",
                         c);
            return false;
        }
    }
    return true;
}
