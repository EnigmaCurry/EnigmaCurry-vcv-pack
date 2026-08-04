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
// rack.hpp deliberately does NOT re-export the free `getPlugin(slug)`
// lookup (only <plugin/Plugin.hpp> and <plugin/Model.hpp>), so pull the
// header in explicitly for the autopatch context-menu action.
#include <plugin.hpp>

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
        configInput(CLK0, "CLK — beat (autopatch: master, 1x)");
        configInput(CLK1, "CLK1 — bar (autopatch: /4)");
        configInput(CLK2, "CLK2 — subdivision (autopatch: x4)");
        configInput(CLK3, "CLK3 — user-defined");
        configOutput(RESET_OUT, "Reset trigger");
        configOutput(RUN_OUT,   "Run gate (±10V, level-sensitive)");
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
        // Bipolar level: +10V when running, -10V when stopped. The 20V
        // swing gives Schmitt-trigger receivers a decisive edge and
        // sits well outside Clocked's level-sensitive threshold band.
        const bool run = g_webbridge_state.runRequested != 0;
        outputs[RUN_OUT].setVoltage(run ? 10.f : -10.f);

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
// Widget — 6HP. All ports live in the RIGHT column so they hug the
// side facing Clocked when the autopatch places Clocked to our right.
// The LEFT column carries the port labels aligned to each row.
//
// Vertical order matches Clocked's own top/bottom split:
//   rows 1..3  → outputs (RUN, RST, BPM) opposite Clocked's top inputs
//   row  4     → visual gap
//   rows 5..8  → inputs  (CLK0..3)        opposite Clocked's bottom outputs
// -------------------------------------------------------------------
#define WB_HP 6
#define WB_ROWS 10
#define WB_COLUMNS 2
static panel_grid<WB_HP, WB_ROWS, WB_COLUMNS> webBridgeGrid;

// -------------------------------------------------------------------
// Autopatch: spawn a Clocked module beside this WebBridge, wire it up,
// and set the two settings this bridge cares about (level-sensitive RUN,
// master flag) plus the two clock dividers (CLK1 = /4, CLK2 = x4).
//
// Clocked port IDs are hardcoded here because its class isn't a public
// header — the numbers come from Cardinal/plugins/ImpromptuModular/src/
// Clocked.cpp enums. If Clocked's enum layout ever changes upstream,
// verify these against that source before shipping.
// -------------------------------------------------------------------
namespace clocked_ids {
    // ParamIds: ENUMS(RATIO_PARAMS, 4) starts at 0. Slot 0 is the master
    // BPM knob; slots 1..3 are sub-clock ratio knobs.
    static constexpr int RATIO_PARAM_CLK1 = 1;
    static constexpr int RATIO_PARAM_CLK2 = 2;
    // InputIds: ENUMS(PW_INPUTS, 4) 0..3, then RESET=4, RUN=5, BPM=6.
    static constexpr int RESET_INPUT = 4;
    static constexpr int RUN_INPUT   = 5;
    static constexpr int BPM_INPUT   = 6;
    // OutputIds: ENUMS(CLK_OUTPUTS, 4) 0..3, then RESET=4, RUN=5, BPM=6.
    static constexpr int CLK_OUTPUT_0 = 0;   // 1..3 are consecutive
    // Ratio knob values map to ratioValues[] in ClockedCommon.hpp:
    // [1, 1.5, 2, 2.5, 3, 4, 5, ..., 16, ...]. Positive knob = multiply,
    // negative = divide. Index 5 = 4, so knob +5 = x4, knob -5 = /4.
    static constexpr float RATIO_DIV_4 = -5.f;
    static constexpr float RATIO_MUL_4 =  5.f;
}

struct EnigmaCurryWebBridgeWidget : ModuleWidget {
    void autopatchClocked() {
        rack::plugin::Plugin* impromptu = rack::plugin::getPlugin("ImpromptuModular");
        if (!impromptu) {
            WARN("WebBridge autopatch: ImpromptuModular pack not loaded");
            return;
        }
        rack::plugin::Model* clockedModel = impromptu->getModel("Clocked");
        if (!clockedModel) {
            WARN("WebBridge autopatch: Clocked model not found in ImpromptuModular");
            return;
        }

        // 1. Instantiate Clocked and add to engine.
        engine::Module* clockedModule = clockedModel->createModule();
        APP->engine->addModule(clockedModule);

        // 2. Set param values: CLK1 /4, CLK2 x4.
        clockedModule->params[clocked_ids::RATIO_PARAM_CLK1].setValue(clocked_ids::RATIO_DIV_4);
        clockedModule->params[clocked_ids::RATIO_PARAM_CLK2].setValue(clocked_ids::RATIO_MUL_4);

        // 3. Set state fields via a synthesized JSON. Field order matters
        //    to avoid a level-sensitive desync:
        //      - "running": false — Clocked defaults `running = true`
        //        after construction. In level-sensitive mode Clocked
        //        only toggles `running` on a Schmitt-detected voltage
        //        *transition* on RUN_INPUT. WebBridge's initial output
        //        is -10V (runRequested==0), which arrives as an
        //        already-low level with no falling edge to detect, so
        //        Clocked would otherwise keep running while the CV
        //        says stopped. Seeding false makes the initial state
        //        agree with the CV.
        //      - "momentaryRunInput": false — level-sensitive RUN.
        //      - "clockMaster": true — becomes the clock master
        //        (dataFromJson calls setAsMaster on this branch).
        if (json_t* stateJ = json_object()) {
            json_object_set_new(stateJ, "running",           json_false());
            json_object_set_new(stateJ, "momentaryRunInput", json_false());
            json_object_set_new(stateJ, "clockMaster",       json_true());
            clockedModule->dataFromJson(stateJ);
            json_decref(stateJ);
        }

        // 4. Create the widget and choose a placement. Placement policy,
        //    with fixed-rack mode in mind, is:
        //      (a) left-flush of WebBridge, gap-search only (no shove)
        //      (b) right-flush of WebBridge, gap-search only
        //      (c) if fixed-rack: snapshot positions, force-shove at
        //          left-flush, keep if every module still fits inside
        //          the region, otherwise restore
        //      (d) last resort: drop just past the current region's
        //          right edge on the same row, marching past any other
        //          defined regions we'd overlap. Not "miles away."
        ModuleWidget* clockedWidget = clockedModel->createModuleWidget(clockedModule);
        if (!clockedWidget) {
            WARN("WebBridge autopatch: createModuleWidget returned null");
            return;
        }
        rack::app::RackWidget* rw = APP->scene->rack;
        // Prefer right-flush: WebBridge's port layout puts all ports on
        // its right edge specifically so cables reach Clocked in a short
        // hop when Clocked sits on our right. Fall back to left-flush
        // only if right doesn't fit.
        const Vec preferredRight = box.pos + Vec(box.size.x, 0);
        const Vec preferredLeft  = box.pos - Vec(clockedWidget->box.size.x, 0);

        bool placed =
            rw->requestModulePos(clockedWidget, preferredRight) ||
            rw->requestModulePos(clockedWidget, preferredLeft);

        if (!placed && rack::settings::rackspaceFixed) {
            // Snapshot every existing module's position so we can undo
            // any shove attempt that would overflow the region.
            std::vector<std::pair<Widget*, Vec>> snap;
            for (Widget* w : rw->getModuleContainer()->children) {
                snap.push_back(std::make_pair(w, w->box.pos));
            }
            const rack::math::Rect region = rack::app::getFiniteRackBox();

            // Two shove candidates on WebBridge's row. setModulePosForce
            // only shoves modules that overlap the target position — a
            // "flush of WebBridge" target sits outside the region and
            // triggers no shove. Anchoring to the region edges puts
            // Clocked squarely on top of everything on the row, forcing
            // them to make way.
            //   (a) region-right minus Clocked width → shoves leftward
            //       (Clocked ends up on the right side of the region —
            //       matches the user-preferred right-of-WebBridge outcome).
            //   (b) region-left → shoves rightward as a fallback.
            const Vec shoveTargets[2] = {
                Vec(region.pos.x + region.size.x - clockedWidget->box.size.x, box.pos.y),
                Vec(region.pos.x, box.pos.y),
            };
            for (int t = 0; t < 2; ++t) {
                // Restore before every attempt.
                for (size_t i = 0; i < snap.size(); ++i)
                    snap[i].first->setPosition(snap[i].second);
                rw->setModulePosForce(clockedWidget, shoveTargets[t]);
                bool allInside = region.contains(clockedWidget->box);
                if (allInside) {
                    for (Widget* w : rw->getModuleContainer()->children) {
                        if (!region.contains(w->box)) { allInside = false; break; }
                    }
                }
                if (allInside) { placed = true; break; }
            }
            if (!placed) {
                for (size_t i = 0; i < snap.size(); ++i)
                    snap[i].first->setPosition(snap[i].second);
            }
        }

        if (!placed) {
            // Drop just past the current region's right edge, on the
            // same row as WebBridge. If defined regions sit further
            // right, march past each so we don't stack inside one.
            Vec outsidePos = box.pos;
            if (rack::settings::rackspaceFixed) {
                outsidePos.x = rack::app::getFiniteRackBox().getRight() + RACK_GRID_WIDTH;
                for (size_t i = 0; i < rack::settings::rackspaceRegions.size(); ++i) {
                    int offHP, offRow, wHP, hRow;
                    if (!rack::settings::resolveRegionBounds((int)i, offHP, offRow, wHP, hRow))
                        continue;
                    rack::math::Rect regionBox;
                    regionBox.pos = RACK_OFFSET + Vec(offHP * RACK_GRID_WIDTH,
                                                     offRow * RACK_GRID_HEIGHT);
                    regionBox.size = Vec(wHP * RACK_GRID_WIDTH, hRow * RACK_GRID_HEIGHT);
                    rack::math::Rect proposed(outsidePos, clockedWidget->box.size);
                    if (regionBox.intersects(proposed)) {
                        outsidePos.x = regionBox.getRight() + RACK_GRID_WIDTH;
                    }
                }
            }
            rw->setModulePosForce(clockedWidget, outsidePos);
        }

        rw->addModule(clockedWidget);

        // 5. Wire cables. Helpers keep it terse.
        auto findInput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getInputs())  if (p->portId == portId) return p;
            return nullptr;
        };
        auto findOutput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getOutputs()) if (p->portId == portId) return p;
            return nullptr;
        };
        auto connect = [](PortWidget* outPort, PortWidget* inPort) -> CableWidget* {
            if (!outPort || !inPort) return nullptr;
            CableWidget* cw = new CableWidget();
            cw->color = APP->scene->rack->getNextCableColor();
            cw->outputPort = outPort;
            cw->inputPort  = inPort;
            cw->updateCable();
            APP->scene->rack->addCable(cw);
            return cw;
        };

        // WebBridge outputs → Clocked inputs.
        connect(findOutput(this, EnigmaCurryWebBridge::RUN_OUT),
                findInput (clockedWidget, clocked_ids::RUN_INPUT));
        connect(findOutput(this, EnigmaCurryWebBridge::RESET_OUT),
                findInput (clockedWidget, clocked_ids::RESET_INPUT));
        connect(findOutput(this, EnigmaCurryWebBridge::BPM_OUT),
                findInput (clockedWidget, clocked_ids::BPM_INPUT));
        // Clocked CLK_OUTPUTS[0..3] → WebBridge CLK0..3.
        for (int i = 0; i < 4; ++i) {
            connect(findOutput(clockedWidget, clocked_ids::CLK_OUTPUT_0 + i),
                    findInput (this,          EnigmaCurryWebBridge::CLK0 + i));
        }

        // 6. History: single ModuleAdd. Cascading cables get cleaned up
        //    by Rack when the module is removed on undo.
        history::ModuleAdd* h = new history::ModuleAdd;
        h->name = "auto-patch Clocked to WebBridge";
        h->setModule(clockedWidget);
        APP->history->push(h);
    }

    // Returns true if any of this WebBridge's ports has a cable to a
    // Clocked module. Used to grey out the autopatch menu item when a
    // Clocked is already wired in — avoids duplicate instantiation and
    // the resulting duelling masters + tangled cables.
    bool isConnectedToClocked() {
        rack::plugin::Plugin* impromptu = rack::plugin::getPlugin("ImpromptuModular");
        if (!impromptu) return false;
        rack::plugin::Model* clockedModel = impromptu->getModel("Clocked");
        if (!clockedModel) return false;

        auto isClockedOnOther = [&](PortWidget* mine, CableWidget* cw) -> bool {
            PortWidget* other = (cw->outputPort == mine) ? cw->inputPort
                                                        : cw->outputPort;
            return other && other->module && other->module->model == clockedModel;
        };
        for (PortWidget* p : getOutputs()) {
            for (CableWidget* cw : APP->scene->rack->getCablesOnPort(p)) {
                if (isClockedOnOther(p, cw)) return true;
            }
        }
        for (PortWidget* p : getInputs()) {
            for (CableWidget* cw : APP->scene->rack->getCablesOnPort(p)) {
                if (isClockedOnOther(p, cw)) return true;
            }
        }
        return false;
    }

    void appendContextMenu(Menu* menu) override {
        menu->addChild(new MenuSeparator);
        const bool alreadyPatched = isConnectedToClocked();
        menu->addChild(createMenuItem(
            alreadyPatched
                ? "Auto-patch Clocked (already connected)"
                : "Auto-patch Clocked (level-sensitive master, CLK1 /4, CLK2 x4)",
            "",
            [this]() { autopatchClocked(); },
            /* disabled */ alreadyPatched
        ));
    }

    EnigmaCurryWebBridgeWidget(EnigmaCurryWebBridge* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/6hp.svg")));

        // All ports in col 1 (right edge). Outputs in the top group,
        // inputs in the bottom group, with row 4 as a visual gap.
        // Vertical order top-to-bottom: RESET, RUN, BPM, (gap), CLK,
        // CLK1, CLK2, CLK3. The RESET/RUN/BPM ordering mirrors the
        // top-left cluster on Clocked so the autopatch cables run
        // straight across without crossing.
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(1, 1), module, EnigmaCurryWebBridge::RESET_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(2, 1), module, EnigmaCurryWebBridge::RUN_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(3, 1), module, EnigmaCurryWebBridge::BPM_OUT));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(5, 1), module, EnigmaCurryWebBridge::CLK0));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(6, 1), module, EnigmaCurryWebBridge::CLK1));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(7, 1), module, EnigmaCurryWebBridge::CLK2));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(8, 1), module, EnigmaCurryWebBridge::CLK3));

        // Labels sit in col 0 on the same row as each port. Output
        // labels use the output-black background convention; input
        // labels use the input-red one, matching the rest of the pack.
        // The first clock input's label is "CLK" (not "CLK0") — the
        // autopatch wires Clocked's 1x master clock here, so it reads
        // as the beat. CLK1/2/3 are numbered subdivisions.
        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(WB_HP);
        overlay->addText("WebBridge", 14, Vec(mm2px(WB_HP * HP_UNIT / 2), 14),
                         WHITE, CLEAR, MANROPE);
        overlay->addText("RESET", 10, webBridgeGrid.loc(1, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("RUN",   10, webBridgeGrid.loc(2, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BPM",   10, webBridgeGrid.loc(3, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("CLK",   10, webBridgeGrid.loc(5, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK1",  10, webBridgeGrid.loc(6, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK2",  10, webBridgeGrid.loc(7, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("CLK3",  10, webBridgeGrid.loc(8, 0),
                         WHITE, RED_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }
};

Model* modelEnigmaCurryWebBridge =
    createModel<EnigmaCurryWebBridge, EnigmaCurryWebBridgeWidget>("WebBridge");
