/*
 * WebBridge — expose a JS-controlled RUN/RESET/BPM triad into the patch,
 * and stream up to four rising-edge clock streams back out to JS. Designed
 * for tight audio↔visual sync in browser-hosted Cardinal: the audio side
 * remains authoritative for musical time; JS follows with latency
 * compensation.
 *
 * IMPORTANT — the C++ class is named EnigmaCurryWebBridge, NOT WebBridge,
 * because DPF's dpf/distrho/src/jackbridge/WebBridge.hpp already defines a
 * global `struct WebBridge : NativeBridge` in the same wasm binary. Two
 * classes with the same name in different TUs violates the C++ ODR and the
 * linker silently merges their vtables — leading to "indirect call to null"
 * / "index out of bounds" the moment Cardinal tries to dispatch a virtual
 * method on this class. The Rack module SLUG remains "WebBridge".
 *
 * Copyright (C) 2026 EnigmaCurry — GPL-3.0-or-later
 *
 * Ports:
 *   OUT RUN    — 0V/10V level gate; wire to Clocked's RUN in (level-sensitive mode)
 *   OUT RESET  — 1ms trigger pulse when JS increments resetEpoch
 *   OUT BPM    — 1V/octave CV around 120 BPM; wire to Clocked's BPM in
 *   IN  CLK0   — beat (Clocked's master out is always 1x)
 *   IN  CLK1/2/3 — composer-defined subdivisions (typically bar/16th/phrase)
 *
 * All state lives in the global `g_webbridge_state`. Two extern-C accessors
 * expose the pointer + size to JS (mirroring the TrackerHost / Mixer8
 * pattern), and the WasmDSP host's EXPORTS list keeps them in the wasm
 * export table.
 */

#include "components.hpp"
#include "plugin.hpp"
#include <cmath>
#include <cstdint>

#ifdef __EMSCRIPTEN__
# include <emscripten.h>
# define WEBBRIDGE_EXPORT EMSCRIPTEN_KEEPALIVE
#else
# define WEBBRIDGE_EXPORT
#endif

// -------------------------------------------------------------------
// Shared state ABI. Layout is fixed so JS can wrap typed-array views
// over the wasm heap at the pointer returned by webbridge_get_ptr().
// All fields explicit-size, natural alignment, static_asserts below
// lock the offsets.
// -------------------------------------------------------------------
#define WEBBRIDGE_EVENT_CAPACITY 256

struct WebBridgeClockEvent {         // 16 bytes
    uint64_t frame;                  // 0  monotonic sample counter at edge
    uint32_t clock;                  // 8  0..3
    uint32_t _pad;                   // 12 padding to 16
};

struct WebBridgeShared {
    // Header — audio thread writes each sample
    uint64_t currentFrame;           // 0   monotonic samples since module ctor
    uint32_t sampleRate;             // 8
    uint32_t _pad0;                  // 12  align to 16
    // JS → audio (JS writes, audio reads)
    uint32_t runRequested;           // 16  0/1
    uint32_t resetEpoch;             // 20  JS increments to fire reset pulse
    float    bpm;                    // 24  plain BPM; module converts to 1V/oct
    uint32_t _pad1;                  // 28  align to 32
    // Audio → JS SPSC ring
    uint32_t eventHead;              // 32  audio writes
    uint32_t eventTail;              // 36  JS writes after drain
    WebBridgeClockEvent events[WEBBRIDGE_EVENT_CAPACITY];  // 40
};

static_assert(sizeof(WebBridgeClockEvent) == 16, "WebBridgeClockEvent layout drift");
static_assert(offsetof(WebBridgeShared, currentFrame) == 0,  "layout: currentFrame");
static_assert(offsetof(WebBridgeShared, sampleRate)   == 8,  "layout: sampleRate");
static_assert(offsetof(WebBridgeShared, runRequested) == 16, "layout: runRequested");
static_assert(offsetof(WebBridgeShared, resetEpoch)   == 20, "layout: resetEpoch");
static_assert(offsetof(WebBridgeShared, bpm)          == 24, "layout: bpm");
static_assert(offsetof(WebBridgeShared, eventHead)    == 32, "layout: eventHead");
static_assert(offsetof(WebBridgeShared, eventTail)    == 36, "layout: eventTail");
static_assert(offsetof(WebBridgeShared, events)       == 40, "layout: events");

alignas(16) WebBridgeShared g_webbridge_state = {
    /* currentFrame */ 0,
    /* sampleRate   */ 48000,
    /* _pad0        */ 0,
    /* runRequested */ 0,
    /* resetEpoch   */ 0,
    /* bpm          */ 120.0f,
    /* _pad1        */ 0,
    /* eventHead    */ 0,
    /* eventTail    */ 0,
    /* events       */ {},
};

extern "C" {
WEBBRIDGE_EXPORT void* webbridge_get_ptr(void)  { return &g_webbridge_state; }
WEBBRIDGE_EXPORT int   webbridge_shared_size(void) { return (int)sizeof(g_webbridge_state); }
}

// -------------------------------------------------------------------
// Module — see file header for the naming rationale (why not "WebBridge").
// -------------------------------------------------------------------
struct EnigmaCurryWebBridge : Module {
    enum ParamIds { NUM_PARAMS };
    enum InputIds  { CLK0, CLK1, CLK2, CLK3, NUM_INPUTS };
    enum OutputIds { RUN_OUT, RESET_OUT, BPM_OUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    // Hysteretic edge detectors per clock input (>=8V high, <=2V low).
    bool clockHigh[4] = {false, false, false, false};
    uint32_t lastSeenResetEpoch = 0;
    dsp::PulseGenerator resetPulse;

    EnigmaCurryWebBridge() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(CLK0, "Clock 0 (master beat)");
        configInput(CLK1, "Clock 1");
        configInput(CLK2, "Clock 2");
        configInput(CLK3, "Clock 3");
        configOutput(RUN_OUT,   "Run gate (0/10V)");
        configOutput(RESET_OUT, "Reset trigger");
        configOutput(BPM_OUT,   "BPM CV (1V/oct, ref=120)");
        lastSeenResetEpoch = g_webbridge_state.resetEpoch;
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        g_webbridge_state.sampleRate = (uint32_t)e.sampleRate;
    }

    void process(const ProcessArgs& args) override {
        // ---- Header ----------------------------------------------------
        g_webbridge_state.currentFrame++;
        const uint64_t frame = g_webbridge_state.currentFrame;

        // ---- JS → audio: RUN ------------------------------------------
        const bool run = g_webbridge_state.runRequested != 0;
        outputs[RUN_OUT].setVoltage(run ? 10.f : 0.f);

        // ---- JS → audio: RESET (epoch → pulse) ------------------------
        const uint32_t epoch = g_webbridge_state.resetEpoch;
        if (epoch != lastSeenResetEpoch) {
            lastSeenResetEpoch = epoch;
            resetPulse.trigger(1e-3f);
        }
        const bool resetHigh = resetPulse.process(args.sampleTime);
        outputs[RESET_OUT].setVoltage(resetHigh ? 10.f : 0.f);

        // ---- JS → audio: BPM (plain BPM → 1V/oct around 120) ----------
        const float bpm = g_webbridge_state.bpm;
        const float bpmCv = (bpm > 1.f) ? std::log2(bpm / 120.f) : -6.f;
        outputs[BPM_OUT].setVoltage(bpmCv);

        // ---- Audio → JS: rising edges on CLK0..CLK3 -------------------
        for (int i = 0; i < 4; ++i) {
            const float v = inputs[CLK0 + i].getVoltage();
            if (!clockHigh[i] && v >= 8.f) {
                clockHigh[i] = true;
                const uint32_t head = g_webbridge_state.eventHead;
                WebBridgeClockEvent& ev = g_webbridge_state.events[head % WEBBRIDGE_EVENT_CAPACITY];
                ev.frame = frame;
                ev.clock = (uint32_t)i;
                ev._pad  = 0;
                g_webbridge_state.eventHead = head + 1;
            } else if (clockHigh[i] && v <= 2.f) {
                clockHigh[i] = false;
            }
        }
    }
};

// -------------------------------------------------------------------
// Widget — 6HP, two columns.
//   Col 0 (module outputs, driven by JS): RUN, RESET, BPM
//   Col 1 (module inputs,  read to JS):   CLK0, CLK1, CLK2, CLK3
// -------------------------------------------------------------------
#define WB_HP 6
#define WB_ROWS 10
#define WB_COLUMNS 2
static panel_grid<WB_HP, WB_ROWS, WB_COLUMNS> webBridgeGrid;

struct EnigmaCurryWebBridgeWidget : ModuleWidget {
    EnigmaCurryWebBridgeWidget(EnigmaCurryWebBridge* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/6hp.svg")));

        // Col 0: outputs (RUN, RESET, BPM) on rows 2, 4, 6.
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(2, 0), module, EnigmaCurryWebBridge::RUN_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(4, 0), module, EnigmaCurryWebBridge::RESET_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(6, 0), module, EnigmaCurryWebBridge::BPM_OUT));

        // Col 1: inputs (CLK0..CLK3) on rows 2, 4, 6, 8.
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(2, 1), module, EnigmaCurryWebBridge::CLK0));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(4, 1), module, EnigmaCurryWebBridge::CLK1));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(6, 1), module, EnigmaCurryWebBridge::CLK2));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(8, 1), module, EnigmaCurryWebBridge::CLK3));

        // Static labels.
        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(WB_HP);
        overlay->addText("WebBridge", 12, Vec(mm2px(WB_HP * HP_UNIT / 2), 12),
                         WHITE, CLEAR, MANROPE);
        overlay->addText("RUN",   9, webBridgeGrid.loc(2, 0).minus(Vec(0, 13)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("RST",   9, webBridgeGrid.loc(4, 0).minus(Vec(0, 13)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BPM",   9, webBridgeGrid.loc(6, 0).minus(Vec(0, 13)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("CLK0",  9, webBridgeGrid.loc(2, 1).minus(Vec(0, 13)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK1",  9, webBridgeGrid.loc(4, 1).minus(Vec(0, 13)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK2",  9, webBridgeGrid.loc(6, 1).minus(Vec(0, 13)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK3",  9, webBridgeGrid.loc(8, 1).minus(Vec(0, 13)),
                         WHITE, RED_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }
};

Model* modelEnigmaCurryWebBridge =
    createModel<EnigmaCurryWebBridge, EnigmaCurryWebBridgeWidget>("WebBridge");
