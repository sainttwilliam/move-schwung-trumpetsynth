/**
 * ui.js — Trumpet Synth UI
 *
 * Screen is 128 × 64 px (monochrome OLED).
 * Encoder 0 cycles between display pages.
 */

'use strict';

let detectedHz = 0.0;
let gate       = false;
let paramPage  = 0;

const PAGE_COUNT = 3; /* 0=overview  1=oscillator  2=filter */

globalThis.init = function () {};

globalThis.tick = function () {
    detectedHz = parseFloat(host_module_get_param('detected_hz') || '0');
    gate       = host_module_get_param('gate') === '1';
    drawScreen();
};

globalThis.onEncoderDelta = function (enc, delta) {
    if (enc === 0)
        paramPage = ((paramPage + delta) % PAGE_COUNT + PAGE_COUNT) % PAGE_COUNT;
};

globalThis.onMidiMessageInternal = function (_msg) {};
globalThis.onMidiMessageExternal = function (_msg) {};

/* -------------------------------------------------------------------------
 * Display
 * ---------------------------------------------------------------------- */
function drawScreen() {
    clear_screen();

    print(0, 0, 'TRUMPET SYNTH', 1);
    draw_line(0, 8, 127, 8, 1);

    /* Gate indicator — filled square when pitch locked */
    if (gate) fill_rect(118, 1, 9, 6, 1);
    else      draw_rect(118, 1, 9, 6, 1);

    /* Detected pitch */
    const hzStr = detectedHz > 0 ? detectedHz.toFixed(1) + ' Hz' : '--- Hz';
    print(0, 11, hzStr, 1);
    if (detectedHz > 0) print(70, 11, hzToNoteName(detectedHz), 1);

    if      (paramPage === 0) drawOverview();
    else if (paramPage === 1) drawOscPage();
    else                      drawFilterPage();

    /* Page dots */
    for (let i = 0; i < PAGE_COUNT; i++) {
        if (i === paramPage) fill_rect(58 + i * 7, 60, 4, 4, 1);
        else                 draw_rect(58 + i * 7, 60, 4, 4, 1);
    }
}

function drawOverview() {
    const wave   = host_module_get_param('osc_wave') || 'saw';
    const vol    = Math.round(parseFloat(host_module_get_param('volume') || '0.8') * 100);
    const conf = parseFloat(host_module_get_param('pitch_confidence') || '0.8').toFixed(2);
    const gate = parseFloat(host_module_get_param('gate_threshold')   || '0.01').toFixed(3);
    print(0, 22, 'OSC  ' + wave.toUpperCase(), 1);
    print(0, 32, 'GATE ' + gate,               1);
    print(0, 42, 'VOL  ' + vol + '%',           1);
}

function drawOscPage() {
    print(0, 21, 'OSCILLATOR', 1);
    print(0, 31, 'Wave  ' + (host_module_get_param('osc_wave')   || '-'),         1);
    print(0, 41, 'Det   ' + (host_module_get_param('osc_detune') || '-') + ' c',  1);
    print(0, 51, 'Lvl   ' + (host_module_get_param('osc_level')  || '-'),         1);
}

function drawFilterPage() {
    print(0, 21, 'FILTER', 1);
    const cutoff = parseFloat(host_module_get_param('filter_cutoff') || '2000');
    const res    = parseFloat(host_module_get_param('filter_resonance') || '0.3');
    print(0, 31, 'Cut  ' + Math.round(cutoff) + ' Hz', 1);
    print(0, 41, 'Res  ' + res.toFixed(2),              1);
}

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */
const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

function hzToNoteName(hz) {
    if (hz <= 0) return '---';
    const midi = Math.round(12 * Math.log2(hz / 440.0) + 69);
    const name = NOTE_NAMES[((midi % 12) + 12) % 12];
    const oct  = Math.floor(midi / 12) - 1;
    return name + oct;
}
