/*
 * Mixer8 — 8 mono inputs → stereo (L/R) with JS-driven gain + pan +
 * two stereo aux buses (SEND / RETURN) for post-fader/post-pan FX.
 *
 * Copyright (C) 2026 EnigmaCurry — GPL-3.0-or-later
 *
 * Per-channel gain[8] and pan[8] live in a plain-old-C global,
 * `g_mixer_ctrl[34]`, laid out as:
 *   [0..7]   gain0..gain7
 *   [8..15]  pan0..pan7
 *   [16..23] sendA0..sendA7   (channel → aux bus A level)
 *   [24..31] sendB0..sendB7   (channel → aux bus B level)
 *   [32]     returnA          (aux bus A → master mix level)
 *   [33]     returnB          (aux bus B → master mix level)
 * The values are updated externally (typically from JS via the
 * `cardinal_get_mixer_ctrl` extern that lives in the WasmDSP host)
 * — this module just reads them on the DSP thread.
 *
 * Pan is equal-power via cos/sin. Since JS updates the control block
 * at Cardinal-block rate (~86 Hz), we don't recompute the gain
 * coefficients per sample — we snap them once every CACHE_STRIDE
 * samples (~2.9 ms @ 44.1 kHz), which is orders of magnitude finer
 * than the source data actually changes.
 *
 * Sends are post-fader AND post-pan — the send signal is the channel's
 * contribution to the master mix, scaled by the per-channel send level.
 * Returns are summed into the master mix scaled by the per-lane return
 * level. Per-channel send levels default to 0.0 (silent — you dial in
 * which channels contribute to each aux bus); return levels default to
 * 1.0 (unity — a patched return is immediately audible in the master).
 *
 * No knobs, no menu items — control is entirely external. The face
 * plate shows 8 input jacks in col 0, 8 send/return jacks in col 1
 * (lane-grouped: SL,SR,RL,RR for A then B), and stereo master out
 * (L/R) in col 2.
 */

#include "components.hpp"
#include "plugin.hpp"
#include <cmath>

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
# define MIXER8_EXPORT EMSCRIPTEN_KEEPALIVE
#else
# define MIXER8_EXPORT
#endif

// -------------------------------------------------------------------
// Shared control block. Defined once here; extern-declared by any
// host that wants to expose it to JS (see
// Cardinal/src/CardinalWasmDSP/CardinalWasmDSP.cpp).
//
// alignas(16) so a Float32Array view lands on a natural boundary.
// -------------------------------------------------------------------
alignas(16) float g_mixer_ctrl[34] = {
    // gains 0..7 default to 1.0
    1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
    // pans 0..7 default to 0.0 (center)
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
    // sendA 0..7 default to 0.0 (silent — sends must be dialed in per channel)
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
    // sendB 0..7 default to 0.0
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
    // returnA, returnB default to 1.0 (unity — patched return audible)
    1.f, 1.f,
};

// C-callable accessors, exported to JS from any wasm variant that links
// Mixer8.cpp (CardinalWasmDSP AND CardinalMini). Because CardinalMini's
// standalone wasm build uses -sMAIN_MODULE (all symbols preserved) and
// CardinalWasmDSP's Makefile lists these two names in -sEXPORTED_FUNCTIONS,
// both are reachable as Module._cardinal_get_mixer_ctrl / _mixer_ctrl_size.
extern "C" {
MIXER8_EXPORT float* cardinal_get_mixer_ctrl(void) { return g_mixer_ctrl; }
MIXER8_EXPORT int    cardinal_mixer_ctrl_size(void) { return (int)sizeof(g_mixer_ctrl); }
}

struct EnigmaCurryMixer8 : Module {
    enum ParamIds { NUM_PARAMS };
    enum InputIds  {
        IN0, IN1, IN2, IN3, IN4, IN5, IN6, IN7,
        RET_AL, RET_AR,   // aux A stereo return
        RET_BL, RET_BR,   // aux B stereo return
        NUM_INPUTS
    };
    enum OutputIds {
        OUT_L, OUT_R,
        SEND_AL, SEND_AR, // aux A stereo send
        SEND_BL, SEND_BR, // aux B stereo send
        NUM_OUTPUTS
    };
    enum LightIds  { NUM_LIGHTS };

    // Cached equal-power pan coefficients + send levels. Refreshed every
    // CACHE_STRIDE samples from g_mixer_ctrl. This avoids ~350k cos/sin
    // calls per second at 44.1 kHz.
    static constexpr int CACHE_STRIDE = 128;
    float leftGain[8]  = {1,1,1,1,1,1,1,1};
    float rightGain[8] = {0,0,0,0,0,0,0,0};
    float sendA[8]     = {1,1,1,1,1,1,1,1};
    float sendB[8]     = {1,1,1,1,1,1,1,1};
    float returnA = 1.f, returnB = 1.f;
    int cacheCounter = 0;

    EnigmaCurryMixer8() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int ch = 0; ch < 8; ++ch) {
            char label[16];
            std::snprintf(label, sizeof(label), "Channel %d", ch + 1);
            configInput(IN0 + ch, label);
        }
        configInput(RET_AL, "Return A left");
        configInput(RET_AR, "Return A right");
        configInput(RET_BL, "Return B left");
        configInput(RET_BR, "Return B right");
        configOutput(OUT_L, "Left");
        configOutput(OUT_R, "Right");
        configOutput(SEND_AL, "Send A left");
        configOutput(SEND_AR, "Send A right");
        configOutput(SEND_BL, "Send B left");
        configOutput(SEND_BR, "Send B right");
        refreshGains();
    }

    void refreshGains() {
        for (int ch = 0; ch < 8; ++ch) {
            const float g   = g_mixer_ctrl[ch];
            const float pan = clamp(g_mixer_ctrl[8 + ch], -1.f, 1.f);
            // pan ∈ [-1, +1] → angle ∈ [0, π/2]
            const float angle = (pan + 1.f) * (float)(M_PI * 0.25);
            leftGain[ch]  = g * std::cos(angle);
            rightGain[ch] = g * std::sin(angle);
            sendA[ch] = g_mixer_ctrl[16 + ch];
            sendB[ch] = g_mixer_ctrl[24 + ch];
        }
        returnA = g_mixer_ctrl[32];
        returnB = g_mixer_ctrl[33];
    }

    void process(const ProcessArgs& args) override {
        if (--cacheCounter <= 0) {
            refreshGains();
            cacheCounter = CACHE_STRIDE;
        }
        float L = 0.f, R = 0.f;
        float sAL = 0.f, sAR = 0.f, sBL = 0.f, sBR = 0.f;
        for (int ch = 0; ch < 8; ++ch) {
            const float s  = inputs[IN0 + ch].getVoltage();
            const float sL = s * leftGain[ch];   // post-fader-post-pan L
            const float sR = s * rightGain[ch];  // post-fader-post-pan R
            L   += sL;
            R   += sR;
            sAL += sL * sendA[ch];
            sAR += sR * sendA[ch];
            sBL += sL * sendB[ch];
            sBR += sR * sendB[ch];
        }
        outputs[SEND_AL].setVoltage(sAL);
        outputs[SEND_AR].setVoltage(sAR);
        outputs[SEND_BL].setVoltage(sBL);
        outputs[SEND_BR].setVoltage(sBR);

        // Mix returns into master. Unpatched inputs read 0V, so an
        // unused aux bus contributes nothing regardless of returnX gain.
        L += inputs[RET_AL].getVoltage() * returnA;
        R += inputs[RET_AR].getVoltage() * returnA;
        L += inputs[RET_BL].getVoltage() * returnB;
        R += inputs[RET_BR].getVoltage() * returnB;

        outputs[OUT_L].setVoltage(L);
        outputs[OUT_R].setVoltage(R);
    }
};

#define HP 10
#define ROWS 10
#define COLUMNS 3
static panel_grid<HP, ROWS, COLUMNS> mixer8Grid;

struct EnigmaCurryMixer8Widget : ModuleWidget {
    EnigmaCurryMixer8Widget(EnigmaCurryMixer8* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/10hp.svg")));

        // Col 0 rows 1..8: 8 channel inputs.
        for (int ch = 0; ch < 8; ++ch) {
            addInput(createInputCentered<PJ301MPort>(
                mixer8Grid.loc(1 + ch, 0), module,
                EnigmaCurryMixer8::IN0 + ch));
        }

        // Col 1 rows 1..8: two lane-grouped aux buses.
        // Lane A (rows 1..4): SEND L, SEND R, RET L, RET R
        // Lane B (rows 5..8): SEND L, SEND R, RET L, RET R
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(1, 1), module, EnigmaCurryMixer8::SEND_AL));
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(2, 1), module, EnigmaCurryMixer8::SEND_AR));
        addInput(createInputCentered<PJ301MPort>(
            mixer8Grid.loc(3, 1), module, EnigmaCurryMixer8::RET_AL));
        addInput(createInputCentered<PJ301MPort>(
            mixer8Grid.loc(4, 1), module, EnigmaCurryMixer8::RET_AR));
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(5, 1), module, EnigmaCurryMixer8::SEND_BL));
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(6, 1), module, EnigmaCurryMixer8::SEND_BR));
        addInput(createInputCentered<PJ301MPort>(
            mixer8Grid.loc(7, 1), module, EnigmaCurryMixer8::RET_BL));
        addInput(createInputCentered<PJ301MPort>(
            mixer8Grid.loc(8, 1), module, EnigmaCurryMixer8::RET_BR));

        // Col 2 rows 3,6: stereo master out (matches lane A/B boundaries).
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(3, 2), module, EnigmaCurryMixer8::OUT_L));
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(6, 2), module, EnigmaCurryMixer8::OUT_R));

        // Static labels: module name + tiny channel numbers + aux tokens
        // + master L/R marks. Sends use output-black bg, returns use
        // input-red bg so the eye distinguishes them without decoding
        // the letters.
        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(HP);
        overlay->addText("Mixer8", 14, Vec(mm2px(HP * HP_UNIT / 2), 12),
                         WHITE, CLEAR, MANROPE);
        for (int ch = 0; ch < 8; ++ch) {
            char label[4];
            std::snprintf(label, sizeof(label), "%d", ch + 1);
            overlay->addText(label, 10,
                             mixer8Grid.loc(1 + ch, 0).minus(Vec(0, 12)),
                             WHITE, RED_TRANSPARENT);
        }
        // Lane A: SL,SR (send=black), RL,RR (return=red)
        overlay->addText("AL", 9, mixer8Grid.loc(1, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("AR", 9, mixer8Grid.loc(2, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("AL", 9, mixer8Grid.loc(3, 1).minus(Vec(0, 12)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("AR", 9, mixer8Grid.loc(4, 1).minus(Vec(0, 12)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("BL", 9, mixer8Grid.loc(5, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BR", 9, mixer8Grid.loc(6, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BL", 9, mixer8Grid.loc(7, 1).minus(Vec(0, 12)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("BR", 9, mixer8Grid.loc(8, 1).minus(Vec(0, 12)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("L", 10, mixer8Grid.loc(3, 2).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("R", 10, mixer8Grid.loc(6, 2).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }
};

Model* modelEnigmaCurryMixer8 =
    createModel<EnigmaCurryMixer8, EnigmaCurryMixer8Widget>("Mixer8");
