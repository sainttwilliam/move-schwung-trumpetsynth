/**
 * ui_chain.js — Trumpet Synth Signal Chain UI shim
 *
 * Loaded by Schwung when the module is used inside a Signal Chain patch.
 * Exports a `chain_ui` object — do NOT override globalThis.init() here.
 *
 * The shim maps the four hardware knobs to the most performance-relevant
 * parameters so players can tweak the synth live while in a chain.
 *
 *   Knob 0 → filter_cutoff
 *   Knob 1 → filter_reso
 *   Knob 2 → comp_threshold
 *   Knob 3 → volume
 */

'use strict';

/* -------------------------------------------------------------------------
 * Knob → parameter mapping
 * ---------------------------------------------------------------------- */
const KNOB_PARAMS = [
    { key: 'filter_cutoff',  min: 20,  max: 20000, label: 'CUT' },
    { key: 'filter_reso',    min: 0.0, max: 1.0,   label: 'RES' },
    { key: 'comp_threshold', min: 0.1, max: 1.0,   label: 'THR' },
    { key: 'volume',         min: 0.0, max: 1.0,   label: 'VOL' },
];

function knobValueToParam(knobIndex, value0to1) {
    const m = KNOB_PARAMS[knobIndex];
    return m.min + value0to1 * (m.max - m.min);
}

/* -------------------------------------------------------------------------
 * Chain UI callbacks
 * ---------------------------------------------------------------------- */
function init() {
    /* No per-chain initialisation needed */
}

function tick() {
    /* Chain UI does not drive its own display update;
     * the host handles the chain overview screen. */
}

function onMidiMessageInternal(_msg) { /* not used in chain shim */ }
function onMidiMessageExternal(_msg) { /* not used in chain shim */ }

function onKnobChange(knobIndex, value0to1) {
    if (knobIndex < 0 || knobIndex >= KNOB_PARAMS.length) return;
    const paramValue = knobValueToParam(knobIndex, value0to1);
    host_module_set_param(KNOB_PARAMS[knobIndex].key, String(paramValue));
}

/* -------------------------------------------------------------------------
 * Export
 * ---------------------------------------------------------------------- */
globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal,
    onKnobChange,
};
