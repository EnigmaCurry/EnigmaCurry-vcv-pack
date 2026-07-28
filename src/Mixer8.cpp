/*
 * Mixer8 — 8 mono inputs → stereo (L/R) with JS-driven gain + pan.
 *
 * Copyright (C) 2026 EnigmaCurry — GPL-3.0-or-later OR MIT
 *
 * Per-channel gain[8] and pan[8] live in a plain-old-C global,
 * `g_mixer_ctrl[16]`, laid out as [gain0..gain7, pan0..pan7].
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
 * No knobs, no menu items — control is entirely external. The face
 * plate shows 8 input jacks in the left column and 2 output jacks
 * (L, R) in the right column.
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
alignas(16) float g_mixer_ctrl[16] = {
    // gains 0..7 default to 1.0
    1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f,
    // pans 0..7 default to 0.0 (center)
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
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
    enum InputIds  { IN0, IN1, IN2, IN3, IN4, IN5, IN6, IN7, NUM_INPUTS };
    enum OutputIds { OUT_L, OUT_R, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    // Cached equal-power pan coefficients. Refreshed every CACHE_STRIDE
    // samples from g_mixer_ctrl. This avoids ~350k cos/sin calls per
    // second at 44.1 kHz.
    static constexpr int CACHE_STRIDE = 128;
    float leftGain[8]  = {1,1,1,1,1,1,1,1};
    float rightGain[8] = {0,0,0,0,0,0,0,0};
    int cacheCounter = 0;

    EnigmaCurryMixer8() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(IN0, "Channel 1");
        configInput(IN1, "Channel 2");
        configInput(IN2, "Channel 3");
        configInput(IN3, "Channel 4");
        configInput(IN4, "Channel 5");
        configInput(IN5, "Channel 6");
        configInput(IN6, "Channel 7");
        configInput(IN7, "Channel 8");
        configOutput(OUT_L, "Left");
        configOutput(OUT_R, "Right");
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
        }
    }

    void process(const ProcessArgs& args) override {
        if (--cacheCounter <= 0) {
            refreshGains();
            cacheCounter = CACHE_STRIDE;
        }
        float L = 0.f, R = 0.f;
        for (int ch = 0; ch < 8; ++ch) {
            const float s = inputs[IN0 + ch].getVoltage();
            L += s * leftGain[ch];
            R += s * rightGain[ch];
        }
        outputs[OUT_L].setVoltage(L);
        outputs[OUT_R].setVoltage(R);
    }
};

#define HP 6
#define ROWS 10
#define COLUMNS 2
static panel_grid<HP, ROWS, COLUMNS> mixer8Grid;

struct EnigmaCurryMixer8Widget : ModuleWidget {
    EnigmaCurryMixer8Widget(EnigmaCurryMixer8* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/6hp.svg")));

        // 8 inputs in column 0 (rows 1..8 → skip 0 and 9 for screw margins).
        // 2 outputs in column 1 at rows 3 and 6 (visually centered against
        // input group).
        for (int ch = 0; ch < 8; ++ch) {
            Vec p = mixer8Grid.loc(1 + ch, 0);
            addInput(createInputCentered<PJ301MPort>(
                p, module, EnigmaCurryMixer8::IN0 + ch));
        }
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(3, 1), module, EnigmaCurryMixer8::OUT_L));
        addOutput(createOutputCentered<PJ301MPort>(
            mixer8Grid.loc(6, 1), module, EnigmaCurryMixer8::OUT_R));

        // Static labels: module name + tiny channel numbers + L/R marks.
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
        overlay->addText("L", 10, mixer8Grid.loc(3, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("R", 10, mixer8Grid.loc(6, 1).minus(Vec(0, 12)),
                         WHITE, BLACK_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }
};

Model* modelEnigmaCurryMixer8 =
    createModel<EnigmaCurryMixer8, EnigmaCurryMixer8Widget>("Mixer8");
