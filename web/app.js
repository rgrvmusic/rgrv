/* SPDX-License-Identifier: MIT */
/*
 * app.js -- WebMIDI playback for the WebAssembly parser.
 *
 * The C side parses the phrase; this side owns the clock and the MIDI output.
 * Timestamped messages keep timing steady when browser timers hiccup.
 */
(() => {
    'use strict';

    const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    const LOOKAHEAD_MS = 220;
    const SCHEDULER_MS = 25;
    const START_DELAY_MS = 80;
    const PREVIEW_DEBOUNCE_MS = 120;
    const MAX_UI_ELEMENTS = 128;

    const state = {
        module: null,
        midi: null,
        midiError: '',
        outputId: '',
        output: null,
        playing: false,
        playingChannels: [],
        stepTicks: 120,
        stepsPerBar: 16,
        baseNote: 60,
        notes: [],
        notesByStep: new Map(),
        steps: 0,
        loopStart: 0,
        loopEnd: 0,
        hasLoop: false,
        clamped: false,
        kick: new Array(16).fill(0),
        snare: new Array(16).fill(0),
        onceSteps: 16,
        stepMs: 125,
        startMs: 0,
        nextStepMs: 0,
        nextStep: 0,
        schedulerId: 0,
        rafId: 0,
        stopTimer: 0,
        previewTimer: 0,
        lastPosition: -1,
        phrase: ''
    };

    const $ = (id) => document.getElementById(id);

    /* Startup ----------------------------------------------------------- */

    async function init() {
        wireEvents();
        renderStepGrid();
        populateOutputs();
        registerServiceWorker();

        try {
            if (typeof createRGRV !== 'function') {
                throw new Error('wasm/rgrv.js is missing. Run "make wasm" first');
            }
            state.module = await createRGRV({
                locateFile: (path) => `wasm/${path}`
            });

            state.stepTicks = state.module._rgrv_web_step_ticks() || 120;
            state.stepsPerBar = state.module._rgrv_web_steps_per_bar() || 16;
            state.baseNote = state.module._rgrv_web_base_note() || 60;
            state.kick = new Array(state.stepsPerBar).fill(0);
            state.snare = new Array(state.stepsPerBar).fill(0);
            renderStepGrid();

            updatePreview();
        } catch (err) {
            $('parserState').textContent = 'WASM failed';
            setStatus(
                `Could not load the WebAssembly parser: ${err.message}. ` +
                'Build it with "make wasm".',
                'error'
            );
        } finally {
            updateTransportButtons();
        }
    }

    function registerServiceWorker() {
        if (!('serviceWorker' in navigator)) return;
        navigator.serviceWorker.register('./service-worker.js').catch(() => {
            // Offline caching is optional; playback works without it.
        });
    }

    function openMenu(open) {
        const menu = $('menu');
        menu.classList.toggle('open', open);
        $('menu-overlay').classList.toggle('active', open);
        $('menu-btn').setAttribute('aria-expanded', String(open));
        menu.setAttribute('aria-hidden', String(!open));
        // Keep the closed menu out of the tab order.
        menu.toggleAttribute('inert', !open);
    }

    function showScene(name) {
        for (const scene of document.querySelectorAll('.scene')) {
            scene.classList.toggle('active', scene.id === `scene-${name}`);
        }
        for (const item of document.querySelectorAll('.nav-item')) {
            item.classList.toggle('active', item.dataset.scene === name);
        }
        openMenu(false);
    }

    function wireEvents() {
        $('menu-btn').addEventListener('click', () => {
            openMenu(!$('menu').classList.contains('open'));
        });
        $('menu-close').addEventListener('click', () => openMenu(false));
        $('menu-overlay').addEventListener('click', () => openMenu(false));
        for (const item of document.querySelectorAll('.nav-item')) {
            item.addEventListener('click', () => showScene(item.dataset.scene));
        }
        openMenu(false);

        $('playBtn').addEventListener('click', () => {
            startPlayback().catch((err) => {
                setStatus(`Could not start: ${err.message}`, 'error');
            });
        });
        $('stopBtn').addEventListener('click', () => stopPlayback('Stopped.'));
        $('downloadBtn').addEventListener('click', downloadMidi);

        $('phraseInput').addEventListener('input', () => {
            if (state.playing) stopPlayback('Stopped: phrase changed.');
            schedulePreview();
        });

        for (const id of ['bpmInput', 'octaveInput', 'transposeInput',
            'gateInput', 'holdInput']) {
            $(id).addEventListener('change', handleMusicalChange);
        }
        for (const id of ['chordToggle', 'loopToggle', 'channelSelect']) {
            $(id).addEventListener('change', handleMusicalChange);
        }
        for (const id of ['kickInput', 'snareInput']) {
            $(id).addEventListener('input', () => {
                if (state.playing) stopPlayback('Stopped: drum pattern changed.');
                schedulePreview();
            });
        }

        document.querySelectorAll('.chip').forEach((chip) => {
            chip.addEventListener('click', () => {
                $('phraseInput').value = chip.dataset.phrase || '';
                if (chip.dataset.chord === '1') $('chordToggle').checked = true;
                handleMusicalChange();
                $('phraseInput').focus();
            });
        });

        $('midiOutput').addEventListener('change', () => {
            if (state.playing) stopPlayback('Stopped: output changed.');
            state.outputId = $('midiOutput').value;
            state.output = currentOutput();
            updateConnection();
            updateReadouts();
        });

        $('refreshMidi').addEventListener('click', () => {
            initMidiAccess({ notify: true }).catch(() => {});
        });

        document.addEventListener('keydown', (event) => {
            if (event.key === 'Escape' &&
                $('menu').classList.contains('open')) {
                event.preventDefault();
                openMenu(false);
                return;
            }
            if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
                event.preventDefault();
                startPlayback().catch((err) => {
                    setStatus(`Could not start: ${err.message}`, 'error');
                });
            }
        });

        window.addEventListener('pagehide', () => stopPlayback());
    }

    function handleMusicalChange() {
        if (state.playing) stopPlayback('Stopped: settings changed.');
        updatePreview();
    }

    function schedulePreview() {
        clearTimeout(state.previewTimer);
        state.previewTimer = setTimeout(updatePreview, PREVIEW_DEBOUNCE_MS);
    }

    /* Parsing ----------------------------------------------------------- */

    function readOptions() {
        return {
            bpm: numberValue('bpmInput', 20, 999, 120),
            octave: numberValue('octaveInput', -5, 5, 0),
            transpose: numberValue('transposeInput', -24, 24, 0),
            gate: numberValue('gateInput', 1, 100, 85),
            hold: numberValue('holdInput', 1, 64, 4),
            chord: $('chordToggle').checked,
            kick: $('kickInput').value.trim(),
            snare: $('snareInput').value.trim()
        };
    }

    function numberValue(id, min, max, fallback) {
        const raw = $(id).value.trim();
        if (raw === '') return fallback;
        const value = Number(raw);
        if (!Number.isFinite(value)) return fallback;
        return Math.min(max, Math.max(min, Math.trunc(value)));
    }

    function buildPhrase() {
        if (!state.module) return false;

        const opts = readOptions();
        let ok = 0;
        try {
            ok = state.module.ccall(
                'rgrv_web_build',
                'number',
                ['string', 'number', 'number', 'number', 'number', 'number',
                    'string', 'string'],
                [$('phraseInput').value, opts.octave, opts.transpose, opts.gate,
                    opts.chord ? 1 : 0, opts.hold, opts.kick, opts.snare]
            );
        } catch (err) {
            clearParsed();
            setStatus(`Could not parse the phrase: ${err.message}`, 'error');
            return false;
        }

        if (!ok) {
            clearParsed();
            const message = state.module.UTF8ToString(
                state.module._rgrv_web_error()
            );
            setStatus(message || 'No playable notes.', 'error');
            return false;
        }

        readParsedPhrase();
        paintLane();
        renderNotes();
        return true;
    }

    function readParsedPhrase() {
        const module = state.module;
        const count = module._rgrv_web_note_count();
        const notes = [];

        for (let i = 0; i < count; i++) {
            notes.push({
                tick: module._rgrv_web_note_tick(i),
                dur: module._rgrv_web_note_dur(i),
                note: module._rgrv_web_note_number(i),
                velocity: module._rgrv_web_note_velocity(i)
            });
        }

        state.notes = notes;
        state.notesByStep = new Map();
        for (const note of notes) {
            const step = Math.floor(note.tick / state.stepTicks);
            if (!state.notesByStep.has(step)) state.notesByStep.set(step, []);
            state.notesByStep.get(step).push(note);
        }

        state.steps = module._rgrv_web_steps();
        state.loopStart = module._rgrv_web_loop_start();
        state.loopEnd = module._rgrv_web_loop_end();
        state.hasLoop = !!module._rgrv_web_has_loop();
        state.clamped = !!module._rgrv_web_clamped();
        state.onceSteps = state.steps || state.stepsPerBar;

        state.kick = [];
        state.snare = [];
        for (let i = 0; i < state.stepsPerBar; i++) {
            state.kick.push(module._rgrv_web_kick(i));
            state.snare.push(module._rgrv_web_snare(i));
        }

        state.phrase = normalizePhrase($('phraseInput').value);
    }

    function clearParsed() {
        state.notes = [];
        state.notesByStep = new Map();
        state.steps = 0;
        state.loopStart = 0;
        state.loopEnd = 0;
        state.hasLoop = false;
        state.clamped = false;
        state.onceSteps = state.stepsPerBar;
        state.kick = new Array(state.stepsPerBar).fill(0);
        state.snare = new Array(state.stepsPerBar).fill(0);
        paintLane();
        $('noteStrip').replaceChildren();
    }

    function updatePreview() {
        clearTimeout(state.previewTimer);
        if (!state.module) return;
        const ok = buildPhrase();
        updateReadouts();
        if (!ok) return;
        if (state.playing) return;

        const count = state.notes.length;
        const steps = state.steps;
        let message = `Ready: ${count} note${count === 1 ? '' : 's'}`;
        if (steps > 0) {
            message += `, ${steps} sixteenth${steps === 1 ? '' : 's'}`;
        }
        if (state.hasLoop) {
            message += ` · loop steps ${state.loopStart + 1}–${state.loopEnd}`;
        }
        if (state.clamped) {
            message += ' · warning: a note was folded into 0..127';
        }
        setStatus(message, 'info');
    }

    function normalizePhrase(text) {
        const collapsed = text.replace(/\s+/g, ' ').trim();
        return collapsed || '(drums)';
    }

    /* Download ---------------------------------------------------------- */

    function slugFromPhrase(text) {
        const letters = text.match(/[A-Za-z]+/);
        if (letters) return letters[0].toLowerCase().slice(0, 127);
        const digits = text.match(/[0-9]+/);
        if (digits) return digits[0].slice(0, 127);
        return 'rgrv';
    }

    function downloadMidi() {
        if (!state.module) {
            setStatus('The WebAssembly parser is still loading.', 'error');
            return;
        }
        if (!buildPhrase()) return;

        const opts = readOptions();
        const selected = $('channelSelect').value;
        const channel = selected === 'all' ? 0 : Number(selected);
        const name = slugFromPhrase($('phraseInput').value);
        const ok = state.module.ccall(
            'rgrv_web_build_midi',
            'number',
            ['number', 'number', 'number', 'string'],
            [opts.bpm, channel, 1, name]
        );

        if (!ok) {
            const message = state.module.UTF8ToString(
                state.module._rgrv_web_error()
            );
            setStatus(message || 'Could not build the .mid file.', 'error');
            return;
        }

        const ptr = state.module._rgrv_web_midi_ptr();
        const size = state.module._rgrv_web_midi_size();
        const bytes = state.module.HEAPU8.slice(ptr, ptr + size);
        try {
            const blob = new Blob([bytes], { type: 'audio/midi' });
            const url = URL.createObjectURL(blob);
            const link = document.createElement('a');
            link.href = url;
            link.download = `${name}.mid`;
            link.click();
            setTimeout(() => URL.revokeObjectURL(url), 0);
        } catch (err) {
            setStatus(`Could not create the download: ${err.message}`, 'error');
            return;
        }

        setStatus(
            `We have regrooved '${state.phrase}' to ${name}.mid`,
            'success'
        );
    }

    /* Visuals ----------------------------------------------------------- */

    function renderStepGrid() {
        const grid = $('stepGrid');
        grid.replaceChildren();
        for (let i = 0; i < state.stepsPerBar; i++) {
            const cell = document.createElement('div');
            cell.className = 'step' + (i % 4 === 0 ? ' beat' : '');
            const index = document.createElement('span');
            index.className = 'step-index';
            index.textContent = String(i + 1).padStart(2, '0');
            cell.append(index);
            grid.append(cell);
        }
    }

    function paintLane() {
        const cells = $('stepGrid').children;
        const noteSteps = new Set();
        for (const step of state.notesByStep.keys()) {
            noteSteps.add(((step % state.stepsPerBar) + state.stepsPerBar) %
                state.stepsPerBar);
        }
        for (let i = 0; i < cells.length; i++) {
            cells[i].classList.toggle('has-note', noteSteps.has(i));
            cells[i].classList.toggle('has-kick', !!state.kick[i]);
            cells[i].classList.toggle('has-snare', !!state.snare[i]);
            cells[i].classList.remove('current');
        }
        $('playheadLabel').textContent = '-';
    }

    function renderNotes() {
        const strip = $('noteStrip');
        strip.replaceChildren();

        // One element per distinct group, in first-appearance order. Repeats
        // relight that element instead of adding another copy.
        const steps = [...state.notesByStep.keys()].sort((a, b) => a - b);
        const unique = new Map();
        for (const step of steps) {
            const notes = state.notesByStep.get(step);
            const signature = groupSignature(notes);
            const seen = unique.get(signature);
            if (seen) {
                for (const note of notes) {
                    const chip = seen.find((item) => item.note === note.note);
                    if (chip) {
                        chip.velocity = Math.max(chip.velocity, note.velocity);
                    }
                }
            } else {
                unique.set(signature, notes.map((note) => ({ ...note })));
            }
        }

        if (unique.size === 0) {
            if (state.steps === 0) {
                const empty = document.createElement('span');
                empty.className = 'note-step';
                empty.textContent = 'drums only';
                strip.append(empty);
            }
            return;
        }

        let shown = 0;
        for (const [signature, notes] of unique) {
            if (shown === MAX_UI_ELEMENTS) break;
            const group = document.createElement('div');
            group.className = 'note-group';
            group.dataset.signature = signature;
            for (const note of notes) {
                const chip = document.createElement('span');
                chip.className =
                    'note-chip' + (note.velocity >= 127 ? ' accent' : '');
                chip.textContent = noteName(note.note);
                chip.title = `MIDI ${note.note} · velocity ${note.velocity}`;
                group.append(chip);
            }
            strip.append(group);
            shown++;
        }

        if (unique.size > MAX_UI_ELEMENTS) {
            const more = document.createElement('span');
            more.className = 'note-step';
            more.textContent = `+${unique.size - MAX_UI_ELEMENTS}`;
            strip.append(more);
        }
    }

    function groupSignature(notes) {
        return notes
            .map((note) => note.note)
            .sort((a, b) => a - b)
            .join(',');
    }

    function updatePlayhead(step, position) {
        const grid = $('stepGrid');
        const lane = ((position % state.stepsPerBar) + state.stepsPerBar) %
            state.stepsPerBar;
        for (let i = 0; i < grid.children.length; i++) {
            grid.children[i].classList.toggle('current', i === lane);
        }

        const sounding = state.notesByStep.get(position);
        const signature = sounding ? groupSignature(sounding) : '';
        for (const group of $('noteStrip').children) {
            if (!(group instanceof HTMLElement) || !group.dataset.signature) {
                continue;
            }
            group.classList.toggle('current',
                group.dataset.signature === signature);
        }

        const bar = Math.floor(step / state.stepsPerBar) + 1;
        const beat = (step % state.stepsPerBar) + 1;
        let label = `bar ${bar} · step ${beat}`;
        if (state.steps > 0) {
            label += ` · phrase ${position + 1}/${state.steps}`;
        }
        $('playheadLabel').textContent = label;
    }

    function clearPlayhead() {
        for (const cell of $('stepGrid').children) {
            cell.classList.remove('current');
        }
        for (const group of $('noteStrip').children) {
            if (group instanceof HTMLElement) {
                group.classList.remove('current');
            }
        }
        $('playheadLabel').textContent = '-';
        state.lastPosition = -1;
    }

    function noteName(note) {
        const index = ((note % 12) + 12) % 12;
        const octave = Math.floor(note / 12) - 1;
        return `${NOTE_NAMES[index]}${octave}`;
    }

    function setStatus(text, kind) {
        const status = $('status');
        status.textContent = text;
        status.className = 'message' + (kind ? ` ${kind}` : '');
    }

    function updateReadouts() {
        $('parserState').textContent = state.module
            ? 'WASM ready'
            : 'WASM loading…';

        const summary = $('phraseSummary');
        if (state.notes.length > 0) {
            const notes = state.notes.length;
            const steps = state.steps;
            summary.textContent = `${notes} note${notes === 1 ? '' : 's'}` +
                (steps > 0
                    ? ` · ${steps} step${steps === 1 ? '' : 's'}`
                    : '');
        } else if (state.kick.some(Boolean) || state.snare.some(Boolean)) {
            summary.textContent = 'drums only';
        } else {
            summary.textContent = '-';
        }

        const opts = readOptions();
        const stepMs = Math.round(60000 / opts.bpm / 4);
        $('clockReadout').textContent =
            `${opts.bpm} BPM · 1/16 = ${stepMs} ms`;

        const base = state.baseNote + opts.octave * 12 + opts.transpose;
        $('baseReadout').textContent = `${noteName(base)} · MIDI ${base}`;
        $('gridReadout').textContent =
            `${state.stepsPerBar} steps/bar · ${state.stepTicks} ticks/step`;

        $('destinationReadout').textContent = state.output
            ? `${state.output.name || state.output.id} · ${channelLabel()}`
            : 'No output';
    }

    /* MIDI outputs ------------------------------------------------------ */

    async function initMidiAccess({ notify = false } = {}) {
        if (!navigator.requestMIDIAccess) {
            state.midiError = 'WebMIDI is not available in this browser';
            populateOutputs();
            if (notify) setStatus(state.midiError, 'error');
            return false;
        }
        if (!window.isSecureContext) {
            state.midiError = 'WebMIDI needs localhost or HTTPS';
            populateOutputs();
            if (notify) setStatus(state.midiError, 'error');
            return false;
        }

        try {
            if (!state.midi) {
                state.midi = await navigator.requestMIDIAccess({ sysex: false });
                state.midi.onstatechange = onMidiStateChange;
            }
            state.midiError = '';
            populateOutputs();
            return true;
        } catch (err) {
            state.midi = null;
            state.midiError = err && err.message
                ? `MIDI permission: ${err.message}`
                : 'MIDI permission denied';
            populateOutputs();
            if (notify) setStatus(state.midiError, 'error');
            return false;
        }
    }

    function onMidiStateChange() {
        const previous = state.outputId;
        const output = getOutputs().find((candidate) =>
            candidate.id === previous);
        if (state.playing && previous &&
            (!output || output.state === 'disconnected')) {
            stopPlayback('Stopped: MIDI output disconnected.');
        }
        populateOutputs();
    }

    function getOutputs() {
        return state.midi ? [...state.midi.outputs.values()] : [];
    }

    function populateOutputs() {
        const select = $('midiOutput');
        const outputs = getOutputs();
        const previous = state.outputId || select.value;

        select.replaceChildren();
        if (outputs.length === 0) {
            const option = document.createElement('option');
            option.value = '';
            option.textContent = state.midi
                ? 'No MIDI outputs found'
                : 'Enable WebMIDI…';
            select.append(option);
            select.disabled = true;
            state.outputId = '';
            state.output = null;
            updateConnection();
            updateReadouts();
            return;
        }

        select.disabled = false;
        const preferred = outputs.find((output) =>
            !/through/i.test(output.name || '')) || outputs[0];

        for (const output of outputs) {
            const option = document.createElement('option');
            option.value = output.id;
            option.textContent = output.name || output.id;
            select.append(option);
        }

        const keep = outputs.find((output) => output.id === previous) ||
            preferred;
        state.outputId = keep.id;
        state.output = keep;
        select.value = keep.id;
        updateConnection();
        updateReadouts();
    }

    function currentOutput() {
        return getOutputs().find((output) => output.id === state.outputId) ||
            null;
    }

    function updateConnection() {
        const outputs = getOutputs();
        const indicator = $('connectionIndicator');
        const text = $('connectionText');

        let label = 'MIDI offline';
        let connected = false;

        if (!navigator.requestMIDIAccess) {
            label = 'WebMIDI unavailable';
        } else if (state.midiError) {
            label = 'MIDI unavailable';
        } else if (!state.midi) {
            label = 'MIDI permission needed';
        } else if (outputs.length === 0) {
            label = 'No MIDI outputs';
        } else {
            label = 'MIDI ready';
            connected = true;
        }

        indicator.classList.toggle('connected', connected);
        text.textContent = label;
    }

    function channelList() {
        const value = $('channelSelect').value;
        if (value === 'all') return Array.from({ length: 16 }, (_, i) => i);
        const channel = Number(value);
        if (!Number.isInteger(channel) || channel < 0 || channel > 15) return [0];
        return [channel];
    }

    function channelLabel() {
        const value = $('channelSelect').value;
        if (value === 'all') return 'all channels';
        return `channel ${Number(value) + 1}`;
    }

    /* Playback ---------------------------------------------------------- */

    function isLooping() {
        return $('loopToggle').checked;
    }

    async function startPlayback() {
        if (!state.module) {
            setStatus('The WebAssembly parser is still loading.', 'error');
            return;
        }
        if (state.playing) stopPlayback();

        // A pending preview must not reparse and repaint mid-play.
        clearTimeout(state.previewTimer);

        if (!buildPhrase()) return;
        updateReadouts();

        const ready = await initMidiAccess({ notify: true });
        if (!ready) return;
        populateOutputs();

        state.output = currentOutput();
        if (!state.output) {
            setStatus(
                'No MIDI output. Connect a device, grant MIDI access, then ' +
                'press Refresh.',
                'error'
            );
            return;
        }
        if (state.output.state && state.output.state !== 'connected') {
            setStatus(`${state.output.name || 'MIDI output'} is disconnected.`,
                'error');
            return;
        }

        const opts = readOptions();
        state.stepMs = 60000 / opts.bpm / 4;
        state.onceSteps = state.steps || state.stepsPerBar;
        state.startMs = performance.now() + START_DELAY_MS;
        state.nextStepMs = state.startMs;
        state.nextStep = 0;
        state.stopTimer = 0;
        state.lastPosition = -1;
        state.playingChannels = channelList();
        state.playing = true;

        try {
            state.output.clear();
        } catch (err) {
            // Not every WebMIDI shim implements clear().
        }
        allNotesOff(state.output, state.playingChannels);
        clearPlayhead();
        updateTransportButtons();

        setStatus(
            `We have regrooved '${state.phrase}' to ` +
            `${state.output.name || state.output.id} (${channelLabel()})`,
            'success'
        );

        schedule();
        state.schedulerId = setInterval(schedule, SCHEDULER_MS);
        state.rafId = requestAnimationFrame(animate);
    }

    function schedule() {
        if (!state.playing || !state.output) return;

        const horizon = performance.now() + LOOKAHEAD_MS;
        const channels = state.playingChannels.length
            ? state.playingChannels
            : channelList();
        const tickMs = state.stepMs / state.stepTicks;
        const looping = isLooping();

        while (state.playing && state.nextStepMs < horizon) {
            if (!looping && state.nextStep >= state.onceSteps) break;

            const position = livePosition(state.nextStep);
            const when = state.nextStepMs;
            const group = state.notesByStep.get(position);

            if (group) {
                for (const note of group) {
                    const off = when +
                        Math.max(1, note.dur) * tickMs;
                    for (const channel of channels) {
                        send([0x90 | channel, note.note, note.velocity], when);
                        send([0x80 | channel, note.note, 0], off);
                    }
                }
            }

            const drumStep = state.nextStep % state.stepsPerBar;
            const kick = state.kick[drumStep] || 0;
            const snare = state.snare[drumStep] || 0;
            const drumOff = when + (state.stepTicks / 2) * tickMs;

            if (kick) {
                send([0x99, 36, kick], when);
                send([0x89, 36, 0], drumOff);
            }
            if (snare) {
                send([0x99, 38, snare], when);
                send([0x89, 38, 0], drumOff);
            }

            state.nextStep++;
            state.nextStepMs += state.stepMs;
        }

        if (!looping && state.playing &&
            state.nextStep >= state.onceSteps && !state.stopTimer) {
            const wait = Math.max(0, state.nextStepMs - performance.now()) + 100;
            state.stopTimer = setTimeout(() => {
                state.stopTimer = 0;
                stopPlayback('One pass complete.');
            }, wait);
        }
    }

    function send(message, when) {
        if (!state.playing || !state.output) return;
        try {
            state.output.send(message, when);
        } catch (err) {
            stopPlayback(`MIDI send failed: ${err.message}`);
        }
    }

    function livePosition(step) {
        if (state.steps === 0) return 0;
        if (!state.hasLoop) return step % state.steps;
        if (step < state.steps) return step;
        const length = state.loopEnd - state.loopStart;
        if (length <= 0) return 0;
        return state.loopStart + ((step - state.steps) % length);
    }

    function animate() {
        if (!state.playing) return;

        const elapsed = performance.now() - state.startMs;
        let step = elapsed < 0 ? 0 : Math.floor(elapsed / state.stepMs);
        if (!isLooping() && state.onceSteps > 0 && step >= state.onceSteps) {
            step = state.onceSteps - 1;
        }

        const position = livePosition(step);
        if (position !== state.lastPosition) {
            state.lastPosition = position;
            updatePlayhead(step, position);
        }

        state.rafId = requestAnimationFrame(animate);
    }

    function stopPlayback(message) {
        const wasPlaying = state.playing;
        state.playing = false;

        if (state.schedulerId) {
            clearInterval(state.schedulerId);
            state.schedulerId = 0;
        }
        if (state.rafId) {
            cancelAnimationFrame(state.rafId);
            state.rafId = 0;
        }
        if (state.stopTimer) {
            clearTimeout(state.stopTimer);
            state.stopTimer = 0;
        }

        if (wasPlaying && state.output) {
            try {
                state.output.clear();
            } catch (err) {
                // The all-notes-off below still runs.
            }
            allNotesOff(
                state.output,
                state.playingChannels.length
                    ? state.playingChannels
                    : channelList()
            );
        }

        state.playingChannels = [];
        clearPlayhead();
        updateTransportButtons();
        if (message) setStatus(message, 'info');
    }

    function allNotesOff(output, channels = channelList()) {
        for (const channel of channels) {
            try {
                output.send([0xB0 | channel, 123, 0]);
            } catch (err) {
                // The output may have just disappeared.
            }
        }
    }

    function updateTransportButtons() {
        const play = $('playBtn');
        const stop = $('stopBtn');
        const download = $('downloadBtn');
        play.disabled = state.playing || !state.module;
        stop.disabled = !state.playing;
        download.disabled = !state.module;
        play.textContent = state.playing ? '▶ Playing' : '▶ Regroove';
    }

    /* -------------------------------------------------------------------- */

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
