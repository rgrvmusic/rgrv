RGRV
====


RGRV reads a text phrase and writes or plays a MIDI arpeggio: a .mid file, a
raw MIDI stream, or a live RtMidi port. The files work with
`plsynth`.

```sh
"Blade" | RGRV                 play the default MIDI output port
"Blade" | RGRV --file          write blade.mid
"Techno" | RGRV --channel 1    play a live MIDI port, channel 1
"1,2,3 Techno" | RGRV | amidi  stream raw MIDI until Ctrl+C
```

```
We have regrooved 'Blade' to microKORG2:microKORG2 _ SOUND 24:0 (all channels)
We have regrooved 'Blade' to blade.mid
```


## Build

```sh
make                       # ./build/rgrv
make windows               # ./build-windows/rgrv.exe (mingw-w64)
```

If RtMidi is not installed, CMake fetches 6.0.0 (needs CMake 3.24 or newer).
`-DRGRV_WITH_RTMIDI=OFF` builds file/raw output only.

The build leaves a capitalised `RGRV` symlink beside the binary:

```sh
./build/RGRV "Blade" --file --out blade.mid
```


## Web

The phrase parser also builds as WebAssembly and sends MIDI through WebMIDI.
The page is in `web/`; Emscripten is expected at
`~/Applications/emsdk` (override with `make EMSDK=/path/to/emsdk wasm`).

```sh
make wasm                  # web/wasm/rgrv.js + rgrv.wasm
make serve                 # http://localhost:8080
```

Open the page from localhost or HTTPS; a `file://` URL cannot use WebMIDI.
The browser uses the same grammar, defaults, chord rules and drum patterns as
the command line. Loop is on by default: the phrase plays once, then a
e( ... )` bracket repeats, or the whole phrase repeats when there is no
bracket. Turn Loop off for a single pass. `make wasm` compiles `src/phrase.c`
and the `src/wasm.c` bridge; `web/wasm/` is generated and not committed.


## The phrase

A-Z are chromatic keys from the base note. 1-0 are the ten notes below A:
1 is ten semitones below the base, and 0 is one semitone below it.

| Phrase | Notes |
|--------|-------|
| `1 2 3` | MIDI 50, 51, 52 (ten, nine and eight semitones below middle C) |
| `0 A` | MIDI 59, 60 (the semitone below middle C, then middle C) |
| `A B C` | MIDI 60, 61, 62 (middle C, C#4, D4) |
| `Z` | MIDI 85 (25 semitones above the base) |

The remaining symbols:

| Symbol | Meaning |
|--------|---------|
| `A`-`Z` | one note, one sixteenth step; case does not matter |
| `1`-`0` | one note; the ten chromatic notes leading into `A`, with `0` just below it |
| `,` | one step of rest |
| `.` | two steps of rest |
| `!` | accent the next note (velocity 127) |
| `?` | tie the note before it into the one after it (slide) |
| `(` ... `)` | live output plays the phrase once, then loops only the bracket |
| space, punctuation | ignored, takes no time |

```sh
"Blade" | RGRV            # B L A D E, five sixteenths
"1,2,3 Techno" | RGRV     # D, D# and E below middle C, then Techno
"!A?B" | RGRV             # accented A, held until B
"Listen (this things rocks)" | RGRV --channel 1   # bracket repeats after the first pass
```

The default base note is middle C (MIDI 60). `--octave N` shifts the phrase
by N octaves and `--transpose N` by N semitones. Both are relative to that
default, so negative values move down. `--octave` accepts -5..5 and
`--transpose` -24..24.

```sh
"Blade" | RGRV --octave -1       # one octave below (MIDI 48)
"Blade" | RGRV --octave 1        # one octave above (MIDI 72)
"Blade" | RGRV --transpose 2     # two semitones higher
"Blade" | RGRV --transpose -3    # three lower
```

Transposition applies to every melodic note and chord, never to drums. Notes
outside 0..127 are clamped with a warning.

### Chords

`--chord` builds a major triad on every letter or number (root, major third,
perfect fifth). Each note is a root, so `BLADE` produces five triads. Each
chord lasts `--hold` steps (default 4, a quarter note) and the next root
starts after those steps; `--hold 1` puts one chord on every sixteenth.
`--tempo N` (same clock as `--bpm N`) sets the speed without changing that
spacing.

```sh
"BLADE" | RGRV --chord --once   # five chords, once, then return
"BLADE" | RGRV --chord --file   # write them to a .mid file
```

Live and raw output loop by default. Without `--once` a live chord repeats
until Ctrl+C; `--file` always ends when the file is written.

### File format

Files are format 0, one track, 480 PPQN, on a sixteenth-note grid.
`nts_smf`, the reader behind `plsynth`, refuses a multi-track file. The track
ends on a bar line. Tempo is written for DAWs but `plsynth` ignores it and uses
the device tempo.


## Drums

Drum parts go to MIDI channel 10: kick is note 36, snare 38. Both options take
a count over the bar or an `x`/`o` pattern (`X` is an accent).

```sh
"Blade" | RGRV --bassdrum 4                     # one hit on each beat
"Blade" | RGRV --bassdrum xoooxoooxoooxxoo      # exact 16-step pattern
"Blade" | RGRV --snare 2                        # beats 2 and 4
"Blade" | RGRV --snare 3                        # three hits starting on beat 2
"Blade" | RGRV --snare XoXoXoXoXoXoXoXoXo       # accented pattern
```

A count is 1..16. Kick counts start on step 1; snare counts start on step 5,
so `--snare 2` lands on beats 2 and 4. A pattern shorter than sixteen steps
repeats; longer is cut at the bar.

nts_smf ignores MIDI channels. A file with drum notes loaded into `plsynth`
turns notes 36/38 into part of the arpeggio. Generate drum parts for a
DAW or hardware, and leave the drum options off for `plsynth` arp files.


## Output modes

| Situation | What happens |
|-----------|--------------|
| stdout is a terminal, no destination asked for | plays the default RtMidi output port |
| `--file` | writes `<first-word>.mid` and reports it |
| stdout is a pipe | streams raw MIDI bytes, looping, until Ctrl+C |
| `--channel N` on a terminal | plays channel N live |
| `--out FILE` | always writes that file (`--out -` means raw stdout) |
| `--raw`, `--play` | force the choice |
| `--once` | play one pass, then exit |

Live and raw output loop by default. `--once` turns either into a single
pass.

The default RtMidi port is the first one that is not ALSA's `Midi Through`.
`--port N` selects one explicitly and `--list-ports` prints the numbers; the
report names the port it opened.

A file has one melodic channel (`--channel N`, default 1) plus the drum
channel. `--channel all` is a live-only broadcast of the melodic part to all
sixteen channels, the default for raw/live output, so
`"1,2,3 Techno" | RGRV | amidi` receives the phrase on every channel.

```
We have regrooved 'Blade' to microKORG2:microKORG2 _ SOUND 24:0 (all channels)
We have regrooved 'Blade' to blade.mid
We have regrooved 'Techno' to raw MIDI (all channels)
```

`--verbose` adds the bar count, steps and channels on stderr, which is also
where the report goes for raw output so the MIDI stream on stdout stays clean.


## Options

```
-h, --help              show the help
    --version           version and attribution
    --list-ports        list RtMidi output ports
-o, --out FILE          write FILE ('-' = raw MIDI on stdout)
-n, --name NAME         base name for the file and track
    --bpm N, --tempo N  tempo 20..999 (default 120)
    --channel N|all     melodic channel 1..16, or all live channels
    --port N            RtMidi output port (default: first real port)
    --chord             each letter/number becomes a major triad
    --hold N            chord length in sixteenths (default 4)
    --gate PCT          note length % of a step (default 85)
    --octave N          shift by N octaves, -5..5 (default 0)
    --transpose N       shift by N semitones, -24..24 (default 0)
    --repeat N          repeat the phrase N times in the file
    --bassdrum N|PAT    kick count 1..16, or x/o pattern (channel 10)
    --snare N|PAT       snare count 1..16, or x/o pattern (channel 10)
    --raw / --file / --play   force raw stdout, a .mid file, or live output
    --once              play one pass, then exit (live/raw)
-v, --verbose           details on stderr
```


## License

Written by Gerard Braad (Regroove | RGRV). MIT licensed.
The live MIDI path uses RtMidi (MIT)

