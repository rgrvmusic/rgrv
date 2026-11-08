/* SPDX-License-Identifier: MIT */
/*
 * wasm.c -- C bridge for the WebMIDI page.
 *
 * Keeps the parsed phrase and drums and exposes them through plain getters,
 * so JavaScript reads the same timeline the CLI builds. Compiled only by the
 * WebAssembly build.
 */

#include "rgrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define RGRV_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define RGRV_EXPORT
#endif

static rgrv_phrase g_phrase;
static rgrv_drums g_drums;
static char g_error[256];
static bool g_ready;
static uint8_t *g_midi;
static size_t g_midi_len;

static void drop_phrase(void)
{
    rgrv_phrase_free(&g_phrase);
    memset(&g_drums, 0, sizeof(g_drums));
    free(g_midi);
    g_midi = NULL;
    g_midi_len = 0;
    g_ready = false;
}

static void reset_state(void)
{
    drop_phrase();
    g_error[0] = '\0';
}

static void fail(const char *message)
{
    snprintf(g_error, sizeof(g_error), "%s", message ? message : "unknown error");
}

/* Build ------------------------------------------------------------------ */

RGRV_EXPORT int rgrv_web_build(const char *text, int octave, int transpose,
                               int gate, int chord, int hold,
                               const char *bassdrum, const char *snare)
{
    reset_state();

    if (!text) {
        fail("no phrase");
        return 0;
    }
    if (octave < -5 || octave > 5) {
        fail("octave must be -5..5");
        return 0;
    }
    if (transpose < -24 || transpose > 24) {
        fail("transpose must be -24..24");
        return 0;
    }
    if (gate < 1 || gate > 100) {
        fail("gate must be 1..100");
        return 0;
    }
    if (hold < 1 || hold > 64) {
        fail("hold must be 1..64");
        return 0;
    }

    rgrv_parse_opts opts;
    opts.octave = octave;
    opts.transpose = transpose;
    opts.gate = gate;
    opts.chord = chord != 0;
    opts.chord_hold = hold;

    if (!rgrv_parse_phrase(text, &opts, &g_phrase, g_error,
                           sizeof(g_error))) {
        return 0;
    }

    if (bassdrum && *bassdrum &&
        !rgrv_drum_pattern(bassdrum, false, g_drums.kick, g_error,
                           sizeof(g_error))) {
        drop_phrase();
        return 0;
    }
    if (snare && *snare &&
        !rgrv_drum_pattern(snare, true, g_drums.snare, g_error,
                           sizeof(g_error))) {
        drop_phrase();
        return 0;
    }

    bool any_drum = false;
    for (int i = 0; i < RGRV_STEPS_PER_BAR; i++) {
        if (g_drums.kick[i] || g_drums.snare[i]) {
            any_drum = true;
            break;
        }
    }
    if (g_phrase.count == 0 && !any_drum) {
        fail("no playable notes (A-Z or 1-0 makes notes)");
        rgrv_phrase_free(&g_phrase);
        return 0;
    }

    g_ready = true;
    return 1;
}

/* MIDI file -------------------------------------------------------------- */

RGRV_EXPORT int rgrv_web_build_midi(int bpm, int channel, int repeat,
                                    const char *name)
{
    if (!g_ready) {
        fail("no phrase");
        return 0;
    }
    free(g_midi);
    g_midi = NULL;
    g_midi_len = 0;

    if (bpm < 20 || bpm > 999) {
        fail("tempo must be 20..999");
        return 0;
    }
    if (channel < 0 || channel > 15) {
        fail("channel must be 1..16");
        return 0;
    }

    rgrv_song song;
    if (!rgrv_build_song(&g_phrase, &g_drums, channel, repeat, &song,
                         g_error, sizeof(g_error))) {
        return 0;
    }

    uint8_t *bytes = NULL;
    size_t len = 0;
    const bool ok = rgrv_smf_bytes(&song, name, bpm, &bytes, &len, g_error,
                                   sizeof(g_error));
    rgrv_song_free(&song);
    if (!ok) return 0;

    g_midi = bytes;
    g_midi_len = len;
    return 1;
}

/* Getters ---------------------------------------------------------------- */

RGRV_EXPORT const uint8_t *rgrv_web_midi_ptr(void)
{
    return g_midi;
}

RGRV_EXPORT int rgrv_web_midi_size(void)
{
    return (int)g_midi_len;
}

RGRV_EXPORT const char *rgrv_web_error(void)
{
    return g_error;
}

RGRV_EXPORT const char *rgrv_web_version(void)
{
    return RGRV_VERSION;
}

RGRV_EXPORT int rgrv_web_note_count(void)
{
    return g_ready ? (int)g_phrase.count : 0;
}

RGRV_EXPORT int rgrv_web_note_tick(int index)
{
    if (!g_ready || index < 0 || (size_t)index >= g_phrase.count) return 0;
    return (int)g_phrase.notes[index].tick;
}

RGRV_EXPORT int rgrv_web_note_dur(int index)
{
    if (!g_ready || index < 0 || (size_t)index >= g_phrase.count) return 0;
    return (int)g_phrase.notes[index].dur;
}

RGRV_EXPORT int rgrv_web_note_number(int index)
{
    if (!g_ready || index < 0 || (size_t)index >= g_phrase.count) return 0;
    return g_phrase.notes[index].note;
}

RGRV_EXPORT int rgrv_web_note_velocity(int index)
{
    if (!g_ready || index < 0 || (size_t)index >= g_phrase.count) return 0;
    return g_phrase.notes[index].velocity;
}

RGRV_EXPORT int rgrv_web_steps(void)
{
    return g_ready ? (int)g_phrase.steps : 0;
}

RGRV_EXPORT int rgrv_web_loop_start(void)
{
    return g_ready ? (int)g_phrase.loop_start : 0;
}

RGRV_EXPORT int rgrv_web_loop_end(void)
{
    return g_ready ? (int)g_phrase.loop_end : 0;
}

RGRV_EXPORT int rgrv_web_has_loop(void)
{
    return g_ready && g_phrase.has_loop;
}

RGRV_EXPORT int rgrv_web_clamped(void)
{
    return g_ready && g_phrase.clamped;
}

RGRV_EXPORT int rgrv_web_kick(int step)
{
    if (step < 0 || step >= RGRV_STEPS_PER_BAR) return 0;
    return g_ready ? g_drums.kick[step] : 0;
}

RGRV_EXPORT int rgrv_web_snare(int step)
{
    if (step < 0 || step >= RGRV_STEPS_PER_BAR) return 0;
    return g_ready ? g_drums.snare[step] : 0;
}

RGRV_EXPORT int rgrv_web_step_ticks(void)
{
    return RGRV_STEP_TICKS;
}

RGRV_EXPORT int rgrv_web_steps_per_bar(void)
{
    return RGRV_STEPS_PER_BAR;
}

RGRV_EXPORT int rgrv_web_base_note(void)
{
    return RGRV_BASE_NOTE;
}
