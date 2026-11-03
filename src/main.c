/* SPDX-License-Identifier: MIT */
/*
 * main.c -- RGRV's command line and the choice of destination.
 *
 * The one rule that matters: a phrase on a terminal writes a file, and a
 * phrase into a pipe plays until Ctrl+C.  That is why `"Blade" | RGRV`
 * leaves blade.mid behind while `"1,2,3 Techno" | RGRV | amidi` keeps the
 * bytes coming, and why asking for a channel is the thing that turns live
 * MIDI on.
 */

#include "rgrv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

typedef struct cli {
    bool help;
    bool version;
    bool list_ports;
    bool verbose;
    bool force_raw;
    bool force_file;
    bool force_play;
    bool chord;
    bool once;
    const char *out_path;
    const char *name;
    const char *bassdrum;
    const char *snare;
    int channel; /* -1 = not given, -2 = all, 0..15 = one channel */
    int port;
    int bpm;
    int repeat;
    int gate;
    int octave;
    int transpose;
    int hold;
} cli;

static void usage(FILE *to)
{
    fprintf(to,
            "RGRV " RGRV_VERSION " -- turn a phrase into an arpeggio\n"
            "Copyright (C) 2026 Gerard Braad (Regroove | RGRV). MIT licensed.\n"
            "\n"
            "Usage: RGRV [options] [phrase]\n"
            "       \"phrase\" | RGRV [options]\n"
            "\n"
            "  \"Blade\" | RGRV                 plays the MIDI output port\n"
            "  \"Blade\" | RGRV --file          writes blade.mid\n"
            "  \"1,2,3 Techno\" | RGRV | amidi  plays raw MIDI until Ctrl+C\n"
            "\n"
            "Options:\n"
            "  -h, --help              show this help\n"
            "      --version           show the version\n"
            "      --list-ports        list RtMidi output ports\n"
            "  -o, --out FILE          write FILE ('-' means raw MIDI on stdout)\n"
            "  -n, --name NAME         base name for the file and track\n"
            "      --bpm N             tempo, 20..999 (default 120)\n"
            "      --tempo N           same as --bpm\n"
            "      --channel N|all     melodic channel 1..16, or all live channels\n"
            "      --port N            RtMidi output port (default: first real port)\n"
            "      --chord             each letter/number becomes a major triad\n"
            "      --hold N            chord length in sixteenths (default 4)\n"
            "      --gate PCT          note length %% of a step (default 85)\n"
            "      --octave N          shift by N octaves, -5..5 (default 0)\n"
            "      --transpose N       shift by N semitones, -24..24 (default 0)\n"
            "      --repeat N          repeat the phrase N times in the file\n"
            "      --bassdrum N|PAT    kick count 1..16, or x/o pattern (channel 10)\n"
            "      --snare N|PAT       snare count 1..16, or x/o pattern (channel 10)\n"
            "      --raw               force raw MIDI on stdout\n"
            "      --file              write a .mid file instead of playing\n"
            "      --play              force live RtMidi output\n"
            "      --once              play one pass, then exit (live/raw)\n"
            "  -v, --verbose           report the details on stderr\n"
            "\n"
            "Phrase:\n"
            "  A-Z   the keys from the base note upward, chromatically\n"
            "  1-0   ten chromatic notes leading into A (0 is just below A)\n"
            "  ,     one sixteenth of rest;  .  two sixteenths of rest\n"
            "  !     accent the next note;  ?  tie this note into the next\n"
            "  (..)  the part live output repeats, after the first pass\n"
            "\n"
            "A file is format 0 at %d PPQN; drums are always on channel 10.\n",
            RGRV_PPQN);
}

static void version(void)
{
    printf("RGRV " RGRV_VERSION "\n"
           "Copyright (C) 2026 Gerard Braad (Regroove | RGRV).\n"
           "MIT licensed. Live MIDI uses RtMidi (MIT).\n");
}

static bool parse_int(const char *s, int *out, int lo, int hi)
{
    char *end = NULL;
    long v;
    if (!s || !*s) return false;
    v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static int stdout_is_tty(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout));
#else
    return isatty(STDOUT_FILENO);
#endif
}

static char *copy_string(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static char *read_stream(FILE *f, size_t *out_len)
{
    size_t cap = 1024, len = 0;
    char *p = (char *)malloc(cap);
    if (!p) return NULL;
    for (;;) {
        if (len + 1 >= cap) {
            const size_t grew = cap * 2;
            char *q = (char *)realloc(p, grew);
            if (!q) {
                free(p);
                return NULL;
            }
            p = q;
            cap = grew;
        }
        const size_t got = fread(p + len, 1, cap - len - 1, f);
        len += got;
        if (got == 0) break;
    }
    p[len] = '\0';
    if (out_len) *out_len = len;
    return p;
}

static int ascii_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}

static int ascii_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ascii_digit(int c)
{
    return c >= '0' && c <= '9';
}

static char *join_args(const char **args, int count)
{
    size_t len = 0;
    for (int i = 0; i < count; i++) len += strlen(args[i]) + 1;
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    p[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i != 0) strcat(p, " ");
        strcat(p, args[i]);
    }
    return p;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    size_t start = 0;
    while (start < len && ascii_space((unsigned char)s[start])) start++;
    while (len > start && ascii_space((unsigned char)s[len - 1])) len--;
    if (start) memmove(s, s + start, len - start);
    s[len - start] = '\0';
}

/* The first word made of letters is the file name; "1,2,3 Techno" is
 * techno.mid, not 1.mid. */
static void slug_from_text(const char *text, char *out, size_t out_len)
{
    const char *best = NULL;
    size_t best_len = 0;
    for (const char *p = text; *p; p++) {
        if (!ascii_alpha((unsigned char)*p)) continue;
        best = p;
        while (ascii_alpha((unsigned char)*p)) p++;
        best_len = (size_t)(p - best);
        break;
    }
    if (!best) {
        for (const char *p = text; *p;) {
            if (!ascii_digit((unsigned char)*p)) {
                p++;
                continue;
            }
            best = p;
            best_len = 0;
            while (ascii_digit((unsigned char)*p)) {
                p++;
                best_len++;
            }
            break;
        }
    }
    if (!best) {
        snprintf(out, out_len, "rgrv");
        return;
    }
    if (best_len >= out_len) best_len = out_len - 1;
    for (size_t i = 0; i < best_len; i++) {
        char c = best[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[best_len] = '\0';
}

static bool has_extension(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash);
}

static char *output_path(const cli *c, const char *slug)
{
    const char *base = c->out_path ? c->out_path : c->name;
    char name[128];
    if (!base) {
        snprintf(name, sizeof(name), "%s", slug);
        base = name;
    }
    if (has_extension(base)) return copy_string(base);

    const size_t n = strlen(base) + 5;
    char *p = (char *)malloc(n);
    if (p) snprintf(p, n, "%s.mid", base);
    return p;
}

int main(int argc, char **argv)
{
    cli c;
    memset(&c, 0, sizeof(c));
    c.channel = -1;
    c.port = -1; /* choose the first port that is not Midi Through */
    c.bpm = 120;
    c.repeat = 1;
    c.gate = 85;
    c.octave = 0;
    c.transpose = 0;
    c.hold = 4;

    const char *positional[64];
    int positional_count = 0;
    bool end_of_options = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!end_of_options && strcmp(a, "--") == 0) {
            end_of_options = true;
            continue;
        }
        if (end_of_options || a[0] != '-' || a[1] == '\0') {
            if (positional_count < (int)(sizeof(positional) /
                                         sizeof(positional[0])))
                positional[positional_count++] = a;
            continue;
        }

        const char *raw = a + 1;
        if (*raw == '-') raw++;
        char name[64];
        const char *eq = strchr(raw, '=');
        const size_t name_len =
            eq ? (size_t)(eq - raw) : strlen(raw);
        if (name_len >= sizeof(name)) {
            fprintf(stderr, "rgrv: unknown option '%s'\n", a);
            return 2;
        }
        memcpy(name, raw, name_len);
        name[name_len] = '\0';
        const char *value = eq ? eq + 1 : NULL;

        const bool wants_value =
            strcmp(name, "out") == 0 || strcmp(name, "o") == 0 ||
            strcmp(name, "name") == 0 || strcmp(name, "n") == 0 ||
            strcmp(name, "bpm") == 0 || strcmp(name, "tempo") == 0 ||
            strcmp(name, "channel") == 0 ||
            strcmp(name, "port") == 0 || strcmp(name, "hold") == 0 ||
            strcmp(name, "gate") == 0 || strcmp(name, "octave") == 0 ||
            strcmp(name, "transpose") == 0 || strcmp(name, "repeat") == 0 ||
            strcmp(name, "bassdrum") == 0 || strcmp(name, "snare") == 0;
        if (wants_value && !value) {
            if (i + 1 >= argc) {
                fprintf(stderr, "rgrv: option '--%s' needs a value\n", name);
                return 2;
            }
            value = argv[++i];
        }
        if (!wants_value && value) {
            fprintf(stderr, "rgrv: option '--%s' takes no value\n", name);
            return 2;
        }

        if (strcmp(name, "help") == 0 || strcmp(name, "h") == 0) {
            c.help = true;
        } else if (strcmp(name, "version") == 0) {
            c.version = true;
        } else if (strcmp(name, "list-ports") == 0) {
            c.list_ports = true;
        } else if (strcmp(name, "verbose") == 0 || strcmp(name, "v") == 0) {
            c.verbose = true;
        } else if (strcmp(name, "raw") == 0) {
            c.force_raw = true;
        } else if (strcmp(name, "file") == 0) {
            c.force_file = true;
        } else if (strcmp(name, "play") == 0) {
            c.force_play = true;
        } else if (strcmp(name, "chord") == 0) {
            c.chord = true;
        } else if (strcmp(name, "once") == 0) {
            c.once = true;
        } else if (strcmp(name, "out") == 0 || strcmp(name, "o") == 0) {
            c.out_path = value;
        } else if (strcmp(name, "name") == 0 || strcmp(name, "n") == 0) {
            c.name = value;
        } else if (strcmp(name, "bassdrum") == 0) {
            c.bassdrum = value;
        } else if (strcmp(name, "snare") == 0) {
            c.snare = value;
        } else if (strcmp(name, "bpm") == 0 ||
                   strcmp(name, "tempo") == 0) {
            if (!parse_int(value, &c.bpm, 20, 999)) {
                fprintf(stderr, "rgrv: --bpm/--tempo must be 20..999\n");
                return 2;
            }
        } else if (strcmp(name, "port") == 0) {
            if (!parse_int(value, &c.port, 0, 1024)) {
                fprintf(stderr, "rgrv: --port must be 0 or more\n");
                return 2;
            }
        } else if (strcmp(name, "hold") == 0) {
            if (!parse_int(value, &c.hold, 1, 64)) {
                fprintf(stderr, "rgrv: --hold must be 1..64\n");
                return 2;
            }
        } else if (strcmp(name, "gate") == 0) {
            if (!parse_int(value, &c.gate, 1, 100)) {
                fprintf(stderr, "rgrv: --gate must be 1..100\n");
                return 2;
            }
        } else if (strcmp(name, "octave") == 0) {
            if (!parse_int(value, &c.octave, -5, 5)) {
                fprintf(stderr, "rgrv: --octave must be -5..5\n");
                return 2;
            }
        } else if (strcmp(name, "transpose") == 0) {
            if (!parse_int(value, &c.transpose, -24, 24)) {
                fprintf(stderr, "rgrv: --transpose must be -24..24\n");
                return 2;
            }
        } else if (strcmp(name, "repeat") == 0) {
            if (!parse_int(value, &c.repeat, 1, 64)) {
                fprintf(stderr, "rgrv: --repeat must be 1..64\n");
                return 2;
            }
        } else if (strcmp(name, "channel") == 0) {
            if (strcmp(value, "all") == 0) {
                c.channel = -2;
            } else if (!parse_int(value, &c.channel, 1, 16)) {
                fprintf(stderr, "rgrv: --channel must be 1..16 or 'all'\n");
                return 2;
            } else {
                c.channel -= 1;
            }
        } else {
            fprintf(stderr, "rgrv: unknown option '%s'\n", a);
            return 2;
        }
    }

    if (c.help) {
        usage(stdout);
        return 0;
    }
    if (c.version) {
        version();
        return 0;
    }
    if (c.list_ports) return rgrv_list_ports();

    if (c.force_raw + c.force_file + c.force_play > 1) {
        fprintf(stderr, "rgrv: choose at most one of --raw, --file, --play\n");
        return 2;
    }

    /* The phrase: the remaining arguments, or everything on stdin.  Reading
     * stdin when it is a terminal would just wait for a Ctrl+D that a piped
     * invocation never needs, so that case is an error with a hint. */
    char *text = NULL;
    if (positional_count > 0) {
        text = join_args(positional, positional_count);
    } else {
        text = read_stream(stdin, NULL);
    }
    if (!text) {
        fprintf(stderr, "rgrv: out of memory\n");
        return 1;
    }
    trim(text);
    if (!*text) {
        free(text);
        fprintf(stderr,
                "rgrv: no phrase (try: \"Blade\" | rgrv, or rgrv \"Blade\")\n");
        return 1;
    }

    rgrv_parse_opts popts;
    popts.octave = c.octave;
    popts.transpose = c.transpose;
    popts.gate = c.gate;
    popts.chord = c.chord;
    popts.chord_hold = c.hold;

    rgrv_phrase phrase;
    char err[256];
    if (!rgrv_parse_phrase(text, &popts, &phrase, err, sizeof(err))) {
        fprintf(stderr, "rgrv: %s\n", err);
        free(text);
        return 1;
    }

    rgrv_drums drums;
    memset(&drums, 0, sizeof(drums));
    if (c.bassdrum &&
        !rgrv_drum_pattern(c.bassdrum, false, drums.kick, err, sizeof(err))) {
        fprintf(stderr, "rgrv: --bassdrum: %s\n", err);
        rgrv_phrase_free(&phrase);
        free(text);
        return 1;
    }
    if (c.snare &&
        !rgrv_drum_pattern(c.snare, true, drums.snare, err, sizeof(err))) {
        fprintf(stderr, "rgrv: --snare: %s\n", err);
        rgrv_phrase_free(&phrase);
        free(text);
        return 1;
    }

    bool any_drum = false;
    for (int i = 0; i < RGRV_STEPS_PER_BAR; i++)
        if (drums.kick[i] || drums.snare[i]) any_drum = true;

    if (phrase.count == 0 && !any_drum) {
        fprintf(stderr, "rgrv: no playable notes (A-Z or 1-0 makes notes)\n");
        rgrv_phrase_free(&phrase);
        free(text);
        return 1;
    }
    if (phrase.clamped)
        fprintf(stderr,
                "rgrv: warning: a note was folded into the 0..127 range\n");

    /* Destination. */
    typedef enum { MODE_FILE, MODE_RAW, MODE_PLAY } mode_t;
    mode_t mode;
    if (c.out_path && strcmp(c.out_path, "-") == 0) {
        if (c.force_file || c.force_play) {
            fprintf(stderr, "rgrv: --out - cannot be combined with --file/--play\n");
            rgrv_phrase_free(&phrase);
            free(text);
            return 2;
        }
        mode = MODE_RAW;
    } else if (c.out_path) {
        if (c.force_raw || c.force_play) {
            fprintf(stderr, "rgrv: --out cannot be combined with --raw/--play\n");
            rgrv_phrase_free(&phrase);
            free(text);
            return 2;
        }
        mode = MODE_FILE;
    } else if (c.force_raw) {
        mode = MODE_RAW;
    } else if (c.force_play) {
        mode = MODE_PLAY;
    } else if (c.force_file) {
        mode = MODE_FILE;
    } else if (!stdout_is_tty()) {
        mode = MODE_RAW;
    } else {
        mode = MODE_PLAY;
    }

    if (c.once && mode == MODE_FILE) {
        fprintf(stderr,
                "rgrv: --once is for live/raw output; --file already stops\n");
        rgrv_phrase_free(&phrase);
        free(text);
        return 2;
    }

    char slug[128];
    slug_from_text(text, slug, sizeof(slug));

    int status = 0;
    if (mode == MODE_FILE) {
        if (c.channel == -2) {
            fprintf(stderr,
                    "rgrv: --channel all is for live output; a file has one "
                    "melodic channel\n");
            rgrv_phrase_free(&phrase);
            free(text);
            return 2;
        }
        const int channel = c.channel >= 0 ? c.channel : 0;
        char *path = output_path(&c, slug);
        if (!path) {
            fprintf(stderr, "rgrv: out of memory\n");
            status = 1;
        } else {
            rgrv_song song;
            if (!rgrv_build_song(&phrase, &drums, channel, c.repeat, &song,
                                 err, sizeof(err))) {
                fprintf(stderr, "rgrv: %s\n", err);
                status = 1;
            } else if (!rgrv_write_smf(path, &song, c.name ? c.name : slug,
                                       c.bpm, err, sizeof(err))) {
                fprintf(stderr, "rgrv: %s\n", err);
                status = 1;
            } else {
                printf("We have regrooved '%s' to %s\n", text, path);
                if (c.verbose)
                    fprintf(stderr,
                            "rgrv: %u steps, %lu notes, %u bar(s), %d BPM, "
                            "channel %d%s%s\n",
                            song.steps, (unsigned long)phrase.count,
                            song.steps / RGRV_STEPS_PER_BAR, c.bpm,
                            channel + 1, any_drum ? ", with drums" : "",
                            phrase.has_loop ? ", loop marked" : "");
            }
            rgrv_song_free(&song);
            free(path);
        }
    } else {
        const bool use_rtmidi = (mode == MODE_PLAY);
        const int live_channel =
            (c.channel == -2 || c.channel == -1) ? -1 : c.channel;
        if (c.verbose)
            fprintf(stderr, "rgrv: %u steps, %lu notes, %d BPM%s%s\n",
                    phrase.steps, (unsigned long)phrase.count, c.bpm,
                    any_drum ? ", with drums" : "",
                    phrase.has_loop ? ", loop marked" : "");
        status = rgrv_play(&phrase, &drums, live_channel, c.bpm, c.gate,
                           c.port, use_rtmidi, c.once, text);
    }

    rgrv_phrase_free(&phrase);
    free(text);
    return status;
}
