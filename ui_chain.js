/**
 * ui_chain.js — Trumpet Synth Signal Chain UI shim
 *
 * Hardware knob mapping:
 *   Knob 0 → gate_threshold
 *   Knob 1 → vol_tracking
 *   Knob 2 → filter_cutoff
 *   Knob 3 → filter_resonance
 *   Knob 4 → osc_detune
 */

'use strict';

const KNOB_PARAMS = [
    { key: 'gate_threshold',   min: 0.0,    max: 0.5,     label: 'GATE' },
    { key: 'vol_tracking',     min: 0.0,    max: 1.0,     label: 'DYN'  },
    { key: 'filter_cutoff',    min: 20.0,   max: 20000.0, label: 'CUT'  },
    { key: 'filter_resonance', min: 0.0,    max: 1.0,     label: 'RES'  },
    { key: 'osc_detune',       min: -100.0, max: 100.0,   label: 'DET'  },
];

function init() {}
function tick() {}
function onMidiMessageInternal(_msg) {}
function onMidiMessageExternal(_msg) {}

function onKnobChange(knobIndex, value0to1) {
    if (knobIndex < 0 || knobIndex >= KNOB_PARAMS.length) return;
    const m = KNOB_PARAMS[knobIndex];
    host_module_set_param(m.key, String(m.min + value0to1 * (m.max - m.min)));
}

globalThis.chain_ui = { init, tick, onMidiMessageInternal, onMidiMessageExternal, onKnobChange };
