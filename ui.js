/**
 * ui.js — Trumpet Synth UI
 *
 * Displays current detected pitch, gate state, and a minimal parameter
 * overview on the Move hardware screen. Full parameter editing is handled
 * via the Shadow UI (ui_hierarchy in module.json).
 *
 * Screen is 128 × 64 px (monochrome OLED).
 */

'use strict';

/* -------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------- */
let detectedHz   = 0.0;
let gate         = false;
let paramPage    = 0; /* 0=overview 1=osc 2=filter 3=env 4=lfo */
const PAGE_COUNT = 5;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */
globalThis.init = function () {
    /* Nothing to initialise — parameters are owned by the DSP plugin. */
};

globalThis.tick = function () {
    /* Poll runtime values from the DSP plugin */
    detectedHz = parseFloat(host_module_get_param('detected_hz') || '0');
    gate       = host_module_get_param('gate') === '1';

    drawScreen();
};

/* -------------------------------------------------------------------------
 * Input handling
 * ---------------------------------------------------------------------- */
globalThis.onEncoderDelta = function (enc, delta) {
    /* Encoder 0 (leftmost) cycles through display pages */
    if (enc === 0) {
        paramPage = ((paramPage + delta) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
    }
};

globalThis.onMidiMessageInternal = function (_msg) { /* not used */ };
globalThis.onMidiMessageExternal = function (_msg) { /* not used */ };

/* -------------------------------------------------------------------------
 * Display
 * ---------------------------------------------------------------------- */
function drawScreen() {
    clear_screen();

    /* Title bar */
    print(0, 0, 'TRUMPET SYNTH', 1);
    draw_line(0, 8, 127, 8, 1);

    /* Gate indicator */
    if (gate) {
        fill_rect(118, 1, 9, 6, 1);      /* solid block when gate open */
    } else {
        draw_rect(118, 1, 9, 6, 1);      /* outline when silent */
    }

    /* Detected pitch */
    const hzStr = detectedHz > 0 ? detectedHz.toFixed(1) + ' Hz' : '--- Hz';
    print(0, 11, hzStr, 1);

    /* Note name approximation */
    if (detectedHz > 0) {
        print(70, 11, hzToNoteName(detectedHz), 1);
    }

    /* Parameter overview by page */
    switch (paramPage) {
        case 0: drawOverview();  break;
        case 1: drawOscPage();   break;
        case 2: drawFilterPage();break;
        case 3: drawEnvPage();   break;
        case 4: drawLfoPage();   break;
    }

    /* Page dots */
    for (let i = 0; i < PAGE_COUNT; i++) {
        if (i === paramPage) fill_rect(50 + i * 6, 60, 4, 4, 1);
        else                 draw_rect(50 + i * 6, 60, 4, 4, 1);
    }
}

function drawOverview() {
    const wave   = host_module_get_param('osc_wave')    || 'saw';
    const cutoff = parseFloat(host_module_get_param('filter_cutoff') || '4000').toFixed(0);
    const vol    = parseFloat(host_module_get_param('volume') || '0.8');
    const volPct = Math.round(vol * 100);

    print(0, 22, 'OSC  ' + wave.toUpperCase(), 1);
    print(0, 32, 'CUTF ' + cutoff + ' Hz',      1);
    print(0, 42, 'VOL  ' + volPct + '%',         1);
}

function drawOscPage() {
    print(0, 21, 'OSCILLATOR',                                             1);
    print(0, 31, 'Wave  ' + (host_module_get_param('osc_wave')   || '-'), 1);
    print(0, 41, 'Det   ' + (host_module_get_param('osc_detune') || '-') + ' c', 1);
    print(0, 51, 'Lvl   ' + (host_module_get_param('osc_level')  || '-'), 1);
}

function drawFilterPage() {
    print(0, 21, 'FILTER',                                                         1);
    print(0, 31, 'Cut  ' + (host_module_get_param('filter_cutoff') || '-') + ' Hz', 1);
    print(0, 41, 'Res  ' + (host_module_get_param('filter_reso')   || '-'),          1);
}

function drawEnvPage() {
    print(0, 21, 'ENVELOPE',                                              1);
    print(0, 31, 'A ' + (host_module_get_param('env_attack')  || '-') + 's', 1);
    print(0, 38, 'D ' + (host_module_get_param('env_decay')   || '-') + 's', 1);
    print(0, 45, 'S ' + (host_module_get_param('env_sustain') || '-'),        1);
    print(0, 52, 'R ' + (host_module_get_param('env_release') || '-') + 's', 1);
}

function drawLfoPage() {
    print(0, 21, 'LFO',                                                           1);
    print(0, 31, 'Rate   ' + (host_module_get_param('lfo_rate')   || '-') + ' Hz', 1);
    print(0, 41, 'Depth  ' + (host_module_get_param('lfo_depth')  || '-'),          1);
    print(0, 51, 'Target ' + (host_module_get_param('lfo_target') || '-'),          1);
}

/* -------------------------------------------------------------------------
 * Utility: frequency → nearest note name
 * ---------------------------------------------------------------------- */
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

function hzToNoteName(hz) {
    if (hz <= 0) return '---';
    const midi = Math.round(12 * Math.log2(hz / 440.0) + 69);
    const name = NOTE_NAMES[((midi % 12) + 12) % 12];
    const oct  = Math.floor(midi / 12) - 1;
    return name + oct;
}
