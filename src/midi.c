/* SPDX-License-Identifier: MIT */
/*
 * midi.c -- the output side of RGRV.
 *
 * Three destinations, one timeline.  A Standard MIDI File is flattened into
 * a single format-0 track (nts_smf, the reader plsynth uses, refuses a
 * multi-track file); raw MIDI is the same messages written to stdout with
 * the clock kept here; an RtMidi port is those bytes handed to a device that
 * is listening.  Live output never ends by itself -- it loops the phrase
 * until Ctrl+C -- which is the point: it is an arpeggio source, not a
 * sequence to be loaded.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "rgrv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#if defined(RGRV_HAVE_RTMIDI)
#if defined(__has_include)
#if __has_include(<rtmidi/rtmidi_c.h>)
#include <rtmidi/rtmidi_c.h>
#elif __has_include(<rtmidi_c.h>)
#include <rtmidi_c.h>
#endif
#else
#include <rtmidi/rtmidi_c.h>
#endif
#endif

/* ======================================================================
 * Song: phrase + drums -> tick-sorted events
 * ==================================================================== */

static bool event_push(rgrv_song *s, uint32_t tick, uint8_t status, uint8_t d1,
                       uint8_t d2, uint8_t rank)
{
    if (s->count == s->cap) {
        const size_t cap = s->cap ? s->cap * 2 : 256;
        rgrv_event *grown =
            (rgrv_event *)realloc(s->events, cap * sizeof(*grown));
        if (!grown) return false;
        s->events = grown;
        s->cap = cap;
    }
    rgrv_event *e = &s->events[s->count++];
    e->tick = tick;
    e->msg[0] = status;
    e->msg[1] = d1;
    e->msg[2] = d2;
    e->len = 3;
    e->rank = rank;
    return true;
}

static int event_cmp(const void *pa, const void *pb)
{
    const rgrv_event *a = (const rgrv_event *)pa;
    const rgrv_event *b = (const rgrv_event *)pb;
    if (a->tick < b->tick) return -1;
    if (a->tick > b->tick) return 1;
    if (a->rank < b->rank) return -1;
    if (a->rank > b->rank) return 1;
    return memcmp(a->msg, b->msg, 3);
}

bool rgrv_build_song(const rgrv_phrase *p, const rgrv_drums *d,
                     int channel, int repeat, rgrv_song *out, char *err,
                     size_t errlen)
{
    memset(out, 0, sizeof(*out));
    if (err && errlen) err[0] = '\0';
    if (!p || !d) {
        if (err) snprintf(err, errlen, "no phrase");
        return false;
    }
    if (channel < 0 || channel > 15) {
        if (err) snprintf(err, errlen, "melodic channel must be 1..16");
        return false;
    }
    if (repeat < 1) repeat = 1;

    const uint32_t seq_steps = p->steps;
    const uint64_t played_steps = (uint64_t)seq_steps * (uint64_t)repeat;

    /* A file ends on a bar line: a looping reader that ends on tick 240
     * would leave the last three quarters of its bar silent. */
    uint64_t total_steps = played_steps;
    if (total_steps == 0) total_steps = RGRV_STEPS_PER_BAR;
    total_steps =
        (total_steps + RGRV_STEPS_PER_BAR - 1) / RGRV_STEPS_PER_BAR *
        RGRV_STEPS_PER_BAR;
    if (total_steps > 1000000) {
        if (err) snprintf(err, errlen, "phrase is too long");
        return false;
    }

    const uint32_t seq_ticks = seq_steps * RGRV_STEP_TICKS;
    const uint8_t vel_status = (uint8_t)(0x90 | (channel & 0x0F));

    for (int r = 0; r < repeat; r++) {
        const uint32_t offset = (uint32_t)r * seq_ticks;
        for (size_t i = 0; i < p->count; i++) {
            const rgrv_note *n = &p->notes[i];
            const uint32_t on = n->tick + offset;
            const uint32_t off = on + (n->dur ? n->dur : 1);
            if (!event_push(out, on, vel_status, n->note, n->velocity, 1) ||
                !event_push(out, off, (uint8_t)(0x80 | (channel & 0x0F)),
                            n->note, 0, 0)) {
                if (err) snprintf(err, errlen, "out of memory");
                rgrv_song_free(out);
                return false;
            }
        }
    }

    const uint8_t drum_ch = RGRV_DRUM_CHANNEL;
    const uint32_t drum_dur = RGRV_STEP_TICKS / 2;
    for (uint32_t bar = 0; bar < total_steps; bar += RGRV_STEPS_PER_BAR) {
        for (int k = 0; k < RGRV_STEPS_PER_BAR; k++) {
            const uint32_t on = (bar + (uint32_t)k) * RGRV_STEP_TICKS;
            if (d->kick[k]) {
                if (!event_push(out, on, (uint8_t)(0x90 | drum_ch),
                                RGRV_KICK_NOTE, d->kick[k], 1) ||
                    !event_push(out, on + drum_dur, (uint8_t)(0x80 | drum_ch),
                                RGRV_KICK_NOTE, 0, 0)) {
                    if (err) snprintf(err, errlen, "out of memory");
                    rgrv_song_free(out);
                    return false;
                }
            }
            if (d->snare[k]) {
                if (!event_push(out, on, (uint8_t)(0x90 | drum_ch),
                                RGRV_SNARE_NOTE, d->snare[k], 1) ||
                    !event_push(out, on + drum_dur, (uint8_t)(0x80 | drum_ch),
                                RGRV_SNARE_NOTE, 0, 0)) {
                    if (err) snprintf(err, errlen, "out of memory");
                    rgrv_song_free(out);
                    return false;
                }
            }
        }
    }

    if (out->count == 0) {
        if (err) snprintf(err, errlen, "nothing to play");
        rgrv_song_free(out);
        return false;
    }

    qsort(out->events, out->count, sizeof(*out->events), event_cmp);
    out->end_tick = (uint32_t)total_steps * RGRV_STEP_TICKS;
    out->steps = (uint32_t)total_steps;
    return true;
}

void rgrv_song_free(rgrv_song *s)
{
    if (!s) return;
    free(s->events);
    memset(s, 0, sizeof(*s));
}

/* ======================================================================
 * Standard MIDI File
 * ==================================================================== */

typedef struct byte_buf {
    uint8_t *p;
    size_t len;
    size_t cap;
    bool bad;
} byte_buf;

static void buf_put(byte_buf *b, uint8_t v)
{
    if (b->bad) return;
    if (b->len == b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : 1024;
        uint8_t *grown = (uint8_t *)realloc(b->p, cap);
        if (!grown) {
            b->bad = true;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    b->p[b->len++] = v;
}

static void buf_put_n(byte_buf *b, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) buf_put(b, p[i]);
}

static void buf_var(byte_buf *b, uint32_t v)
{
    uint8_t tmp[4];
    int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7F);
    while ((v >>= 7) != 0) tmp[n++] = (uint8_t)(0x80 | (v & 0x7F));
    while (n > 0) buf_put(b, tmp[--n]);
}

bool rgrv_write_smf(const char *path, const rgrv_song *song, const char *name,
                    int bpm, char *err, size_t errlen)
{
    if (err && errlen) err[0] = '\0';
    if (!path || !song) {
        if (err) snprintf(err, errlen, "no output");
        return false;
    }
    if (bpm < 1 || bpm > 1000) {
        if (err) snprintf(err, errlen, "tempo must be 20..999 BPM");
        return false;
    }

    byte_buf track = {0};
    const char *title = (name && *name) ? name : "RGRV";
    size_t title_len = strlen(title);
    if (title_len > 127) title_len = 127;

    buf_var(&track, 0);
    buf_put(&track, 0xFF);
    buf_put(&track, 0x03);
    buf_var(&track, (uint32_t)title_len);
    buf_put_n(&track, (const uint8_t *)title, title_len);

    /* The tempo is written for other readers; plsynth ignores it and plays
     * at the drumlogue's tempo, which is the right behaviour for an arp. */
    const uint32_t tempo = 60000000u / (uint32_t)bpm;
    buf_var(&track, 0);
    buf_put(&track, 0xFF);
    buf_put(&track, 0x51);
    buf_put(&track, 0x03);
    buf_put(&track, (uint8_t)((tempo >> 16) & 0xFF));
    buf_put(&track, (uint8_t)((tempo >> 8) & 0xFF));
    buf_put(&track, (uint8_t)(tempo & 0xFF));

    buf_var(&track, 0);
    buf_put(&track, 0xFF);
    buf_put(&track, 0x58);
    buf_put(&track, 0x04);
    buf_put(&track, 0x04); /* 4/4 */
    buf_put(&track, 0x02); /* 2^2 = 4 quarter notes */
    buf_put(&track, 0x18); /* 24 MIDI clocks per click */
    buf_put(&track, 0x08); /* 8 thirty-seconds per quarter */

    uint32_t at = 0;
    for (size_t i = 0; i < song->count; i++) {
        const rgrv_event *e = &song->events[i];
        buf_var(&track, e->tick - at);
        at = e->tick;
        buf_put_n(&track, e->msg, e->len);
    }
    buf_var(&track, song->end_tick - at);
    buf_put(&track, 0xFF);
    buf_put(&track, 0x2F);
    buf_put(&track, 0x00);

    if (track.bad) {
        free(track.p);
        if (err) snprintf(err, errlen, "out of memory");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(track.p);
        if (err) snprintf(err, errlen, "cannot write '%s'", path);
        return false;
    }

    const uint8_t head[14] = {
        'M',  'T',  'h',  'd',  0, 0, 0, 6,
        0,    0, /* format 0 */
        0,    1, /* one track */
        (uint8_t)((RGRV_PPQN >> 8) & 0xFF), (uint8_t)(RGRV_PPQN & 0xFF)
    };
    const uint8_t chunk[4] = {'M', 'T', 'r', 'k'};
    const uint8_t len_be[4] = {
        (uint8_t)((track.len >> 24) & 0xFF),
        (uint8_t)((track.len >> 16) & 0xFF),
        (uint8_t)((track.len >> 8) & 0xFF),
        (uint8_t)(track.len & 0xFF)
    };

    bool ok = fwrite(head, 1, sizeof(head), f) == sizeof(head) &&
              fwrite(chunk, 1, sizeof(chunk), f) == sizeof(chunk) &&
              fwrite(len_be, 1, sizeof(len_be), f) == sizeof(len_be) &&
              fwrite(track.p, 1, track.len, f) == track.len;
    if (fclose(f) != 0) ok = false;
    free(track.p);

    if (!ok) {
        if (err) snprintf(err, errlen, "cannot write '%s'", path);
        return false;
    }
    return true;
}

/* ======================================================================
 * Live output: raw bytes on stdout, or an RtMidi port
 * ==================================================================== */

static volatile sig_atomic_t g_running = 1;

static void stop_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

typedef struct out_dev {
    bool rtmidi;
#if defined(RGRV_HAVE_RTMIDI)
    RtMidiOutPtr out;
#endif
} out_dev;

static int out_send(out_dev *o, const uint8_t *msg, size_t len)
{
    if (!o->rtmidi) {
        if (fwrite(msg, 1, len, stdout) != len || fflush(stdout) != 0) {
            /* A closed pipe (`... | head`) is a normal way to stop a raw
             * stream, even when the caller left SIGPIPE ignored.  Stop the
             * loop instead of spinning on a dead descriptor. */
            g_running = 0;
            return -1;
        }
        return 0;
    }
#if defined(RGRV_HAVE_RTMIDI)
    return rtmidi_out_send_message(o->out, msg, (int)len) < 0 ? -1 : 0;
#else
    (void)msg;
    (void)len;
    return -1;
#endif
}

static void out_note(out_dev *o, uint8_t channel, bool on, uint8_t note,
                     uint8_t velocity)
{
    const uint8_t msg[3] = {
        (uint8_t)((on ? 0x90 : 0x80) | (channel & 0x0F)), note, velocity
    };
    out_send(o, msg, sizeof(msg));
}

static void out_all_notes_off(out_dev *o, int channel)
{
    if (channel < 0) {
        for (int ch = 0; ch < 16; ch++) {
            const uint8_t msg[3] = {(uint8_t)(0xB0 | ch), 123, 0};
            out_send(o, msg, sizeof(msg));
        }
    } else {
        const uint8_t msg[3] = {(uint8_t)(0xB0 | (channel & 0x0F)), 123, 0};
        out_send(o, msg, sizeof(msg));
    }
}

static uint64_t now_ns(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64() * 1000000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* Sleep in short slices so a Ctrl+C is noticed within a fiftieth of a
 * second, and keep an absolute target so a late wake-up does not drift the
 * tempo. */
static void sleep_until(uint64_t target)
{
    while (g_running) {
        const uint64_t now = now_ns();
        if (now >= target) return;
        uint64_t delta = target - now;
        if (delta > 50000000ULL) delta = 50000000ULL;
#ifdef _WIN32
        Sleep((DWORD)((delta + 999999ULL) / 1000000ULL));
#else
        struct timespec ts;
        ts.tv_sec = (time_t)(delta / 1000000000ULL);
        ts.tv_nsec = (long)(delta % 1000000000ULL);
        nanosleep(&ts, NULL);
#endif
    }
}

typedef struct active_note {
    uint8_t channel;
    uint8_t note;
    uint64_t off_ns;
} active_note;

typedef struct active_list {
    active_note *v;
    size_t n;
    size_t cap;
} active_list;

static bool active_push(active_list *a, uint8_t channel, uint8_t note,
                        uint64_t off_ns)
{
    if (a->n == a->cap) {
        const size_t cap = a->cap ? a->cap * 2 : 64;
        active_note *grown =
            (active_note *)realloc(a->v, cap * sizeof(*grown));
        if (!grown) return false;
        a->v = grown;
        a->cap = cap;
    }
    a->v[a->n].channel = channel;
    a->v[a->n].note = note;
    a->v[a->n].off_ns = off_ns;
    a->n++;
    return true;
}

static void active_due(active_list *a, out_dev *o, uint64_t now)
{
    size_t write = 0;
    for (size_t i = 0; i < a->n; i++) {
        if (a->v[i].off_ns <= now) {
            out_note(o, a->v[i].channel, false, a->v[i].note, 0);
        } else {
            a->v[write++] = a->v[i];
        }
    }
    a->n = write;
}

static void active_flush(active_list *a, out_dev *o)
{
    for (size_t i = 0; i < a->n; i++)
        out_note(o, a->v[i].channel, false, a->v[i].note, 0);
    a->n = 0;
}

static const char *channel_label(int channel, char *buf, size_t len)
{
    if (channel < 0)
        return "all channels";
    snprintf(buf, len, "channel %d", channel + 1);
    return buf;
}

static uint32_t melody_position(const rgrv_phrase *p, uint64_t step)
{
    if (p->steps == 0) return 0;
    if (!p->has_loop) return (uint32_t)(step % p->steps);
    if (step < p->steps) return (uint32_t)step;
    const uint32_t len = p->loop_end - p->loop_start;
    return p->loop_start + (uint32_t)((step - p->steps) % len);
}

#if defined(RGRV_HAVE_RTMIDI)
static bool open_rtmidi(out_dev *o, int port, char *port_name,
                        size_t port_name_len, char *err, size_t errlen)
{
    RtMidiOutPtr out = rtmidi_out_create_default();
    if (!out || !out->ok) {
        /* RtMidi's message for a failed backend is not reliably a string
         * this C wrapper can print (a missing /dev/snd/seq is one path), so
         * the diagnosis stays here and the raw bytes stay available. */
        if (err)
            snprintf(err, errlen,
                     "cannot start RtMidi (is a MIDI output available?); "
                     "use --raw to write MIDI bytes instead");
        if (out) rtmidi_out_free(out);
        return false;
    }

    const unsigned int count = rtmidi_get_port_count(out);
    if (count == 0) {
        if (err)
            snprintf(err, errlen,
                     "no MIDI output ports (connect one, or pipe raw MIDI)");
        rtmidi_out_free(out);
        return false;
    }
    unsigned int chosen = 0;
    if (port < 0) {
        /* ALSA always exports Midi Through, and playing into it is silent.
         * When no port was asked for, prefer the first real destination. */
        for (unsigned int i = 0; i < count; i++) {
            char name[256] = {0};
            int len = (int)sizeof(name);
            rtmidi_get_port_name(out, i, name, &len);
            if (!strstr(name, "Through")) {
                chosen = i;
                break;
            }
        }
    } else if ((unsigned int)port >= count) {
        if (err)
            snprintf(err, errlen,
                     "MIDI port %d does not exist (0..%u; try --list-ports)",
                     port, count - 1);
        rtmidi_out_free(out);
        return false;
    } else {
        chosen = (unsigned int)port;
    }

    if (port_name && port_name_len) {
        int n = (int)port_name_len;
        port_name[0] = '\0';
        rtmidi_get_port_name(out, chosen, port_name, &n);
    }

    rtmidi_open_port(out, chosen, "RGRV");
    if (!out->ok) {
        if (err)
            snprintf(err, errlen, "cannot open MIDI port %u", chosen);
        rtmidi_out_free(out);
        return false;
    }

    o->rtmidi = true;
    o->out = out;
    return true;
}
#endif

bool rgrv_have_rtmidi(void)
{
#if defined(RGRV_HAVE_RTMIDI)
    return true;
#else
    return false;
#endif
}

int rgrv_list_ports(void)
{
#if defined(RGRV_HAVE_RTMIDI)
    RtMidiOutPtr out = rtmidi_out_create_default();
    if (!out || !out->ok) {
        fprintf(stderr,
                "rgrv: cannot start RtMidi (is a MIDI output available?)\n");
        if (out) rtmidi_out_free(out);
        return 1;
    }
    const unsigned int count = rtmidi_get_port_count(out);
    if (count == 0) {
        printf("No MIDI output ports.\n");
        rtmidi_out_free(out);
        return 0;
    }
    for (unsigned int i = 0; i < count; i++) {
        char name[256] = {0};
        int len = (int)sizeof(name);
        rtmidi_get_port_name(out, i, name, &len);
        printf("%u: %s\n", i, name[0] ? name : "(unnamed)");
    }
    rtmidi_out_free(out);
    return 0;
#else
    fprintf(stderr, "rgrv: built without RtMidi; raw MIDI is still available\n");
    return 1;
#endif
}

int rgrv_play(const rgrv_phrase *p, const rgrv_drums *d, int channel, int bpm,
              int gate, int port, bool use_rtmidi, bool once,
              const char *display)
{
    (void)gate; /* already folded into each note's duration by the parser */

    out_dev o;
    memset(&o, 0, sizeof(o));
    char port_name[256] = {0};
    char chanbuf[32];
    const char *chlabel = channel_label(channel, chanbuf, sizeof(chanbuf));

    if (use_rtmidi) {
#if defined(RGRV_HAVE_RTMIDI)
        char err[256];
        if (!open_rtmidi(&o, port, port_name, sizeof(port_name), err,
                         sizeof(err))) {
            fprintf(stderr, "rgrv: %s\n", err);
            return 1;
        }
        printf("We have regrooved '%s' to %s (%s)\n", display,
               port_name[0] ? port_name : "MIDI port", chlabel);
#else
        (void)port;
        fprintf(stderr,
                "rgrv: this build has no RtMidi; use raw output or --out\n");
        return 1;
#endif
    } else {
#ifdef _WIN32
        /* stdout is a text stream by default; the CR/LF translation would
         * rewrite any 0x0A in a note number.  Raw MIDI must be binary. */
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        setvbuf(stdout, NULL, _IONBF, 0);
        fprintf(stderr, "We have regrooved '%s' to raw MIDI (%s)\n", display,
                chlabel);
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    g_running = 1;

    double tick_sec = 60.0 / ((double)bpm * (double)RGRV_PPQN);
    uint64_t tick_ns = (uint64_t)(tick_sec * 1000000000.0);
    if (tick_ns < 1) tick_ns = 1;
    const uint64_t step_ns = tick_ns * RGRV_STEP_TICKS;
    const uint32_t once_steps =
        p->steps ? p->steps : RGRV_STEPS_PER_BAR;

    active_list active = {0};
    const uint64_t t0 = now_ns();
    uint64_t step = 0;
    uint64_t next_step_ns = t0;
    int first_ch = channel < 0 ? 0 : channel;
    int last_ch = channel < 0 ? 15 : channel;

    while (g_running) {
        const uint64_t now = now_ns();
        active_due(&active, &o, now);

        if (now >= next_step_ns) {
            if (once && step >= once_steps) break;
            const uint64_t on_time = next_step_ns;
            const uint32_t pos = melody_position(p, step);

            if (p->steps > 0) {
                for (size_t i = 0; i < p->count; i++) {
                    const rgrv_note *n = &p->notes[i];
                    if (n->tick / RGRV_STEP_TICKS != pos) continue;
                    const uint64_t off =
                        on_time + (uint64_t)(n->dur ? n->dur : 1) * tick_ns;
                    for (int ch = first_ch; ch <= last_ch; ch++) {
                        out_note(&o, (uint8_t)ch, true, n->note,
                                 n->velocity);
                        active_push(&active, (uint8_t)ch, n->note, off);
                    }
                }
            }

            const int k = (int)(step % RGRV_STEPS_PER_BAR);
            const uint64_t drum_off =
                on_time + (uint64_t)(RGRV_STEP_TICKS / 2) * tick_ns;
            if (d->kick[k]) {
                out_note(&o, RGRV_DRUM_CHANNEL, true, RGRV_KICK_NOTE,
                         d->kick[k]);
                active_push(&active, RGRV_DRUM_CHANNEL, RGRV_KICK_NOTE,
                            drum_off);
            }
            if (d->snare[k]) {
                out_note(&o, RGRV_DRUM_CHANNEL, true, RGRV_SNARE_NOTE,
                         d->snare[k]);
                active_push(&active, RGRV_DRUM_CHANNEL, RGRV_SNARE_NOTE,
                            drum_off);
            }

            step++;
            next_step_ns = t0 + step * step_ns;
            continue;
        }

        uint64_t wake = next_step_ns;
        for (size_t i = 0; i < active.n; i++)
            if (active.v[i].off_ns < wake) wake = active.v[i].off_ns;
        const uint64_t before = now_ns();
        if (wake > before) sleep_until(wake);
    }

    active_flush(&active, &o);
    out_all_notes_off(&o, channel);
    free(active.v);

#if defined(RGRV_HAVE_RTMIDI)
    if (o.rtmidi) {
        rtmidi_close_port(o.out);
        rtmidi_out_free(o.out);
    }
#endif
    return 0;
}
