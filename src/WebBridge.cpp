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
 *   OUT RUN    — 0V/10V level gate; wire to a clock source's RUN in
 *   OUT RESET  — 1ms trigger pulse when JS increments resetEpoch
 *   OUT BPM    — 1V/octave CV around 120 BPM; wire to a BPM CV input
 *   IN  BEAT   — beat trigger (1× the tempo)
 *   IN  BAR    — bar trigger (typically ÷4 of BEAT)
 *   IN  STEP — step trigger (finest musically-meaningful subdivision)
 *   IN  BPM    — 1V/octave CV; presence flips the JS UI into read-only mode
 *
 * The autopatch menu offers two turnkey wirings that fit best-effort with
 * their clock source's port layout: Clocked (level-sensitive master, BAR
 * ÷4, STEP ×4) and Tracker (beat→BEAT, bar→BAR). Manual wiring works
 * too — the port semantics don't enforce either source.
 *
 * All state lives in the global `g_webbridge_state`. Two extern-C accessors
 * expose the pointer + size to JS (mirroring the Mixer8 pattern), and the
 * WasmDSP host's EXPORTS list keeps them in the wasm export table.
 */

#include "components.hpp"
#include "plugin.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

// Port-connection bitmap layout (audio thread → JS). Fixed bit
// positions are part of the shared ABI — do not reorder.
static constexpr uint32_t WB_CONN_BIT_BEAT_IN   = 1u << 0;
static constexpr uint32_t WB_CONN_BIT_BAR_IN    = 1u << 1;
static constexpr uint32_t WB_CONN_BIT_STEP_IN = 1u << 2;
// Bit 3 was CLK3 — retired when the fourth clock input was dropped.
static constexpr uint32_t WB_CONN_MASK_INPUTS   = 0x07u;   // bits 0..2
static constexpr uint32_t WB_CONN_BIT_RUN_OUT   = 1u << 4;
static constexpr uint32_t WB_CONN_BIT_RESET_OUT = 1u << 5;
static constexpr uint32_t WB_CONN_BIT_BPM_OUT   = 1u << 6;
static constexpr uint32_t WB_CONN_MASK_OUTPUTS  = 0x70u;   // bits 4..6
// BPM_IN is a signal input distinct from the trigger inputs — when
// cabled, the audio thread samples its voltage each block, converts it
// back to BPM (Impromptu Clocked convention: bpm = 120·2^V), and
// republishes to JS via bpmReadback. Presence of this bit is what
// switches the JS UI's BPM widget from JS-controlled to read-only mode.
static constexpr uint32_t WB_CONN_BIT_BPM_IN    = 1u << 7;

// Ratio-label buffer: per-input UTF-8 string reported by the
// upstream module's ParamQuantity::getDisplayValueString() (or "×1"
// for the Clocked master). 16 bytes accommodates typical ratio
// glyphs — "×" and "÷" are 2-byte in UTF-8, so a ratio like "÷32"
// takes 4 bytes; 16 gives comfortable headroom for future units.
#define WB_RATIO_LABEL_BYTES 16

struct WebBridgeShared {
    // Header — audio thread writes each sample
    uint64_t currentFrame;           // 0   monotonic samples since module ctor
    uint32_t sampleRate;             // 8
    uint32_t connectionState;        // 12  bitmap of cabled ports; see WB_CONN_* above
    // JS → audio (JS writes, audio reads)
    uint32_t runRequested;           // 16  0/1
    uint32_t resetEpoch;             // 20  JS increments to fire reset pulse
    float    bpm;                    // 24  plain BPM; module converts to 1V/oct
    uint32_t _pad1;                  // 28  align to 32
    // Audio → JS SPSC ring
    uint32_t eventHead;              // 32  audio writes
    uint32_t eventTail;              // 36  JS writes after drain
    WebBridgeClockEvent events[WEBBRIDGE_EVENT_CAPACITY];  // 40..4136
    // UI-thread-written ratio labels, one per trigger input (BEAT / BAR
    // / STEP, in that order). Empty when input is unconnected or the
    // source module didn't declare a name we can format. Null-terminated
    // within the buffer.
    char clockRatioLabels[3][WB_RATIO_LABEL_BYTES];  // 4136..4184
    // Time-index tracker — bar & beat since last reset. Both are
    // 1-indexed. beatsPerBar is written by the widget thread from
    // the BAR-input ratio (÷N → N; fallback 4). The audio thread
    // counts BEAT-input rising edges and advances bar/beat accordingly.
    uint32_t timeBar;                // 4184
    uint32_t timeBeat;               // 4188
    uint32_t beatsPerBar;            // 4192
    // Elapsed run time in seconds since last reset, accumulated only
    // while runRequested is set — paused time doesn't count. Float
    // precision is fine here: at a session length of ~1 hour we still
    // hold sub-millisecond accuracy, well below the mm:ss display's
    // resolution. Reset to 0 by the same epoch handler that snaps
    // the bar/beat cursor back to 1.1.
    float    runElapsedSeconds;      // 4196
    // Audio → JS BPM readback. Written every process() from the BPM_IN
    // port when it's cabled (via bpm = 120·2^V). 0 when disconnected —
    // JS uses the WB_CONN_BIT_BPM_IN bit to know whether this field is
    // meaningful, so no in-band sentinel needed.
    float    bpmReadback;            // 4200
};

static_assert(sizeof(WebBridgeClockEvent) == 16, "WebBridgeClockEvent layout drift");
static_assert(offsetof(WebBridgeShared, currentFrame)     == 0,    "layout: currentFrame");
static_assert(offsetof(WebBridgeShared, sampleRate)       == 8,    "layout: sampleRate");
static_assert(offsetof(WebBridgeShared, connectionState)  == 12,   "layout: connectionState");
static_assert(offsetof(WebBridgeShared, runRequested)     == 16,   "layout: runRequested");
static_assert(offsetof(WebBridgeShared, resetEpoch)       == 20,   "layout: resetEpoch");
static_assert(offsetof(WebBridgeShared, bpm)              == 24,   "layout: bpm");
static_assert(offsetof(WebBridgeShared, eventHead)        == 32,   "layout: eventHead");
static_assert(offsetof(WebBridgeShared, eventTail)        == 36,   "layout: eventTail");
static_assert(offsetof(WebBridgeShared, events)           == 40,   "layout: events");
static_assert(offsetof(WebBridgeShared, clockRatioLabels) == 4136, "layout: clockRatioLabels");
static_assert(offsetof(WebBridgeShared, timeBar)          == 4184, "layout: timeBar");
static_assert(offsetof(WebBridgeShared, timeBeat)         == 4188, "layout: timeBeat");
static_assert(offsetof(WebBridgeShared, beatsPerBar)      == 4192, "layout: beatsPerBar");
static_assert(offsetof(WebBridgeShared, runElapsedSeconds) == 4196, "layout: runElapsedSeconds");
static_assert(offsetof(WebBridgeShared, bpmReadback)      == 4200, "layout: bpmReadback");

alignas(16) WebBridgeShared g_webbridge_state = {
    /* currentFrame      */ 0,
    /* sampleRate        */ 48000,
    /* connectionState   */ 0,
    /* runRequested      */ 0,
    /* resetEpoch        */ 0,
    /* bpm               */ 120.0f,
    /* _pad1             */ 0,
    /* eventHead         */ 0,
    /* eventTail         */ 0,
    /* events            */ {},
    /* clockRatioLabels  */ {{0}, {0}, {0}},
    /* timeBar           */ 1,
    /* timeBeat          */ 1,
    /* beatsPerBar       */ 4,
    /* runElapsedSeconds */ 0.0f,
    /* bpmReadback       */ 0.0f,
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
    enum InputIds  { BEAT_IN, BAR_IN, STEP_IN, BPM_IN, NUM_INPUTS };
    enum OutputIds { RUN_OUT, RESET_OUT, BPM_OUT, NUM_OUTPUTS };
    enum LightIds  { NUM_LIGHTS };

    // Hysteretic edge detectors per trigger input (>=8V high, <=2V low).
    // Index 0=beat, 1=bar, 2=step — same order as the InputIds enum.
    bool clockHigh[3] = {false, false, false};
    uint32_t lastSeenResetEpoch = 0;
    dsp::PulseGenerator resetPulse;
    // Time-index tracking: the display shows "bar.beat" cursor position
    // where the very first BEAT_IN pulse lands us at 1.1 (not 1.2). We
    // gate the first increment on this flag so the initial pulse just
    // marks presence rather than advancing the counter.
    bool firstBeatSeen = false;

    EnigmaCurryWebBridge() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(BEAT_IN,   "Beat trigger");
        configInput(BAR_IN,    "Bar trigger");
        configInput(STEP_IN, "Step trigger");
        configInput(BPM_IN,    "BPM CV in (1V/oct, ref=120) — enables read-only mode");
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

        // Republish the current cable-connectivity bitmap so the JS
        // side can react to plug/unplug without polling the Rack API.
        // isConnected() is a cheap flag lookup (see Port::isConnected in
        // rack/engine/Port.hpp — a bool field, no traversal); we still
        // pay 7 branches + a store per sample, which is a rounding
        // error next to the other work on this thread.
        uint32_t conn = 0;
        if (inputs[BEAT_IN].isConnected())    conn |= WB_CONN_BIT_BEAT_IN;
        if (inputs[BAR_IN].isConnected())     conn |= WB_CONN_BIT_BAR_IN;
        if (inputs[STEP_IN].isConnected())  conn |= WB_CONN_BIT_STEP_IN;
        if (inputs[BPM_IN].isConnected())     conn |= WB_CONN_BIT_BPM_IN;
        if (outputs[RUN_OUT].isConnected())   conn |= WB_CONN_BIT_RUN_OUT;
        if (outputs[RESET_OUT].isConnected()) conn |= WB_CONN_BIT_RESET_OUT;
        if (outputs[BPM_OUT].isConnected())   conn |= WB_CONN_BIT_BPM_OUT;
        g_webbridge_state.connectionState = conn;

        // ---- BPM_IN → JS readback -------------------------------------
        // When cabled, convert the incoming 1V/oct signal back to BPM.
        // Sampling once per block is plenty for a display value; JS
        // polls at ~15Hz. When unconnected we leave the last observed
        // value in place — the WB_CONN_BIT_BPM_IN bit is the authority
        // on whether the readback is meaningful.
        if (inputs[BPM_IN].isConnected()) {
            const float bpmCv = inputs[BPM_IN].getVoltage();
            g_webbridge_state.bpmReadback = 120.f * std::exp2(bpmCv);
        }

        // ---- JS → audio: RUN ------------------------------------------
        // Bipolar level: +10V when running, -10V when stopped. The 20V
        // swing gives Schmitt-trigger receivers a decisive edge and
        // sits well outside Clocked's level-sensitive threshold band.
        const bool run = g_webbridge_state.runRequested != 0;
        outputs[RUN_OUT].setVoltage(run ? 10.f : -10.f);

        // Elapsed run time — only ticks while the transport is running,
        // matching the semantics of the RUN gate driving Clocked. Wall-
        // clock pause time is excluded, which is what a musician cares
        // about when reading the mm:ss counter on the display.
        if (run) {
            g_webbridge_state.runElapsedSeconds += args.sampleTime;
        }

        // ---- JS → audio: RESET (epoch → pulse) ------------------------
        const uint32_t epoch = g_webbridge_state.resetEpoch;
        if (epoch != lastSeenResetEpoch) {
            lastSeenResetEpoch = epoch;
            resetPulse.trigger(1e-3f);
            // Clocked's own reset chain will restart its beat sequence
            // shortly after this pulse arrives, so line up the time
            // index to match: the next BEAT_IN rising edge becomes 1.1.
            g_webbridge_state.timeBar  = 1;
            g_webbridge_state.timeBeat = 1;
            g_webbridge_state.runElapsedSeconds = 0.f;
            firstBeatSeen = false;
        }
        const bool resetHigh = resetPulse.process(args.sampleTime);
        outputs[RESET_OUT].setVoltage(resetHigh ? 10.f : 0.f);

        // ---- JS → audio: BPM (plain BPM → 1V/oct around 120) ----------
        const float bpm = g_webbridge_state.bpm;
        const float bpmCv = (bpm > 1.f) ? std::log2(bpm / 120.f) : -6.f;
        outputs[BPM_OUT].setVoltage(bpmCv);

        // ---- Audio → JS: rising edges on BEAT / BAR / STEP ----------
        // Loop index i matches the InputIds enum ordering (0=beat, 1=bar,
        // 2=step) so the emitted `ev.clock` field acts as a stable
        // channel id for JS to demultiplex on.
        for (int i = 0; i < 3; ++i) {
            const float v = inputs[BEAT_IN + i].getVoltage();
            if (!clockHigh[i] && v >= 8.f) {
                clockHigh[i] = true;
                const uint32_t head = g_webbridge_state.eventHead;
                WebBridgeClockEvent& ev = g_webbridge_state.events[head % WEBBRIDGE_EVENT_CAPACITY];
                ev.frame = frame;
                ev.clock = (uint32_t)i;
                ev._pad  = 0;
                g_webbridge_state.eventHead = head + 1;

                // Time-index tracker — BEAT_IN drives cursor advance;
                // beatsPerBar comes from BAR_IN's ratio via the widget
                // thread. First beat after reset holds the cursor at
                // 1.1 so the display reads "at beat 1 of bar 1" instead
                // of skipping straight to 1.2.
                if (i == 0) {
                    if (!firstBeatSeen) {
                        firstBeatSeen = true;
                    } else {
                        uint32_t bpb = g_webbridge_state.beatsPerBar;
                        if (bpb < 1) bpb = 4;
                        uint32_t nextBeat = g_webbridge_state.timeBeat + 1;
                        if (nextBeat > bpb) {
                            g_webbridge_state.timeBar += 1;
                            g_webbridge_state.timeBeat = 1;
                        } else {
                            g_webbridge_state.timeBeat = nextBeat;
                        }
                    }
                }
            } else if (clockHigh[i] && v <= 2.f) {
                clockHigh[i] = false;
            }
        }
    }
};

// -------------------------------------------------------------------
// Widget — 6HP. All ports live in the RIGHT column so short right-flush
// autopatch cables reach a neighbour placed to our right. The LEFT
// column carries the port labels aligned to each row.
//
// Vertical layout, top to bottom (rows 1..7 of a 10-row grid, rows
// 8..10 intentionally empty as headroom for future ports):
//   row 1   RESET  (out)
//   row 2   RUN    (out)
//   row 3   BPM    (out)
//   row 4   bpm    (in, tempo readback)
//   row 5   beat
//   row 6   bar
//   row 7   step
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
        // Clocked CLK_OUTPUTS[0] (master ×1) → WebBridge BEAT_IN.
        // Clocked CLK_OUTPUTS[1] (÷4 default) → WebBridge BAR_IN.
        // Clocked CLK_OUTPUTS[2] (×4 default) → WebBridge STEP_IN.
        // Clocked CLK_OUTPUTS[3] has no matching input anymore — the
        // fourth trigger input was retired.
        connect(findOutput(clockedWidget, clocked_ids::CLK_OUTPUT_0 + 0),
                findInput (this, EnigmaCurryWebBridge::BEAT_IN));
        connect(findOutput(clockedWidget, clocked_ids::CLK_OUTPUT_0 + 1),
                findInput (this, EnigmaCurryWebBridge::BAR_IN));
        connect(findOutput(clockedWidget, clocked_ids::CLK_OUTPUT_0 + 2),
                findInput (this, EnigmaCurryWebBridge::STEP_IN));

        // 6. History: single ModuleAdd. Cascading cables get cleaned up
        //    by Rack when the module is removed on undo.
        history::ModuleAdd* h = new history::ModuleAdd;
        h->name = "auto-patch Clocked to WebBridge";
        h->setModule(clockedWidget);
        APP->history->push(h);
    }

    // Position `w` next to this WebBridge and add it to the rack.
    // Same policy as autopatchClocked's inline placement:
    //   (a) right-flush of WebBridge, gap-search only
    //   (b) left-flush of WebBridge, gap-search only
    //   (c) in fixed-rack mode: snapshot + shove-from-region-edge
    //   (d) last resort: drop past the current region's right edge on
    //       the same row, marching past neighbouring regions.
    // Used by autopatchTracker; autopatchClocked still has this logic
    // inline (predates the extraction) and could migrate later.
    void placeAdjacent(ModuleWidget* w) {
        rack::app::RackWidget* rw = APP->scene->rack;
        const Vec preferredRight = box.pos + Vec(box.size.x, 0);
        const Vec preferredLeft  = box.pos - Vec(w->box.size.x, 0);
        bool placed =
            rw->requestModulePos(w, preferredRight) ||
            rw->requestModulePos(w, preferredLeft);

        if (!placed && rack::settings::rackspaceFixed) {
            std::vector<std::pair<Widget*, Vec>> snap;
            for (Widget* c : rw->getModuleContainer()->children)
                snap.push_back(std::make_pair(c, c->box.pos));
            const rack::math::Rect region = rack::app::getFiniteRackBox();
            const Vec shoveTargets[2] = {
                Vec(region.pos.x + region.size.x - w->box.size.x, box.pos.y),
                Vec(region.pos.x, box.pos.y),
            };
            for (int t = 0; t < 2; ++t) {
                for (size_t i = 0; i < snap.size(); ++i)
                    snap[i].first->setPosition(snap[i].second);
                rw->setModulePosForce(w, shoveTargets[t]);
                bool allInside = region.contains(w->box);
                if (allInside) {
                    for (Widget* c : rw->getModuleContainer()->children) {
                        if (!region.contains(c->box)) { allInside = false; break; }
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
                    rack::math::Rect proposed(outsidePos, w->box.size);
                    if (regionBox.intersects(proposed))
                        outsidePos.x = regionBox.getRight() + RACK_GRID_WIDTH;
                }
            }
            rw->setModulePosForce(w, outsidePos);
        }

        rw->addModule(w);
    }

    // Returns true if any of this WebBridge's ports has a cable to a
    // module matching (pluginSlug, modelSlug). Used to grey out each
    // autopatch menu item when its target is already wired in — avoids
    // duplicate instantiation and duelling clock masters.
    bool isConnectedToModel(const char* pluginSlug, const char* modelSlug) {
        rack::plugin::Plugin* plug = rack::plugin::getPlugin(pluginSlug);
        if (!plug) return false;
        rack::plugin::Model* model = plug->getModel(modelSlug);
        if (!model) return false;

        auto isOnOther = [&](PortWidget* mine, CableWidget* cw) -> bool {
            PortWidget* other = (cw->outputPort == mine) ? cw->inputPort
                                                        : cw->outputPort;
            return other && other->module && other->module->model == model;
        };
        for (PortWidget* p : getOutputs()) {
            for (CableWidget* cw : APP->scene->rack->getCablesOnPort(p)) {
                if (isOnOther(p, cw)) return true;
            }
        }
        for (PortWidget* p : getInputs()) {
            for (CableWidget* cw : APP->scene->rack->getCablesOnPort(p)) {
                if (isOnOther(p, cw)) return true;
            }
        }
        return false;
    }

    bool isConnectedToClocked() {
        return isConnectedToModel("ImpromptuModular", "Clocked");
    }
    bool isConnectedToTracker() {
        return isConnectedToModel("EnigmaCurry", "Tracker");
    }

    // Autopatch a Tracker module beside this WebBridge and wire:
    //     WB.RUN_OUT   → Tracker.RUN_IN
    //     WB.RESET_OUT → Tracker.RESET_IN
    //     Tracker.OUT_BEAT → WB.BEAT_IN
    //     Tracker.OUT_BAR  → WB.BAR_IN
    //     Tracker.OUT_STEP → WB.STEP_IN
    //     Tracker.OUT_BPM  → WB.BPM_IN
    // BPM_OUT is intentionally left unwired: Tracker's tempo comes from
    // the loaded module file, and the BPM_IN cable is what tells the JS
    // UI to flip the BPM widget into read-only mode.
    //
    // Placement policy mirrors autopatchClocked's: right-flush first
    // (WB.RUN/RESET_OUT sit on WebBridge's right edge → straight cables
    // into Tracker's left-side inputs). The bar/beat return legs loop
    // back across because Tracker's outs are on its own right edge, but
    // that's unavoidable given the two modules' natural port layouts.
    void autopatchTracker() {
        rack::plugin::Plugin* plug = rack::plugin::getPlugin("EnigmaCurry");
        if (!plug) {
            WARN("WebBridge autopatch: EnigmaCurry pack not loaded");
            return;
        }
        rack::plugin::Model* trackerModel = plug->getModel("Tracker");
        if (!trackerModel) {
            WARN("WebBridge autopatch: Tracker model not found");
            return;
        }

        engine::Module* trackerModule = trackerModel->createModule();
        APP->engine->addModule(trackerModule);

        ModuleWidget* trackerWidget = trackerModel->createModuleWidget(trackerModule);
        if (!trackerWidget) {
            WARN("WebBridge autopatch: Tracker createModuleWidget returned null");
            return;
        }

        placeAdjacent(trackerWidget);

        auto findInput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getInputs())  if (p->portId == portId) return p;
            return nullptr;
        };
        auto findOutput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getOutputs()) if (p->portId == portId) return p;
            return nullptr;
        };
        auto connect = [](PortWidget* outPort, PortWidget* inPort) {
            if (!outPort || !inPort) return;
            CableWidget* cw = new CableWidget();
            cw->color = APP->scene->rack->getNextCableColor();
            cw->outputPort = outPort;
            cw->inputPort  = inPort;
            cw->updateCable();
            APP->scene->rack->addCable(cw);
        };

        // Tracker port ids are stable — they live inside the same pack, so
        // enum values are compile-time known via plugin.hpp.
        constexpr int TRACKER_RUN_IN   = 0;   // matches EnigmaCurryTracker::RUN_IN
        constexpr int TRACKER_RESET_IN = 1;   //                     ::RESET_IN
        constexpr int TRACKER_OUT_BPM  = 2;   //                     ::OUT_BPM
        constexpr int TRACKER_OUT_BEAT = 3;   //                     ::OUT_BEAT
        constexpr int TRACKER_OUT_BAR  = 4;   //                     ::OUT_BAR
        constexpr int TRACKER_OUT_STEP = 5;   //                     ::OUT_STEP

        connect(findOutput(this, EnigmaCurryWebBridge::RUN_OUT),
                findInput (trackerWidget, TRACKER_RUN_IN));
        connect(findOutput(this, EnigmaCurryWebBridge::RESET_OUT),
                findInput (trackerWidget, TRACKER_RESET_IN));
        connect(findOutput(trackerWidget, TRACKER_OUT_BEAT),
                findInput (this, EnigmaCurryWebBridge::BEAT_IN));
        connect(findOutput(trackerWidget, TRACKER_OUT_BAR),
                findInput (this, EnigmaCurryWebBridge::BAR_IN));
        connect(findOutput(trackerWidget, TRACKER_OUT_STEP),
                findInput (this, EnigmaCurryWebBridge::STEP_IN));
        connect(findOutput(trackerWidget, TRACKER_OUT_BPM),
                findInput (this, EnigmaCurryWebBridge::BPM_IN));

        history::ModuleAdd* h = new history::ModuleAdd;
        h->name = "auto-patch Tracker to WebBridge";
        h->setModule(trackerWidget);
        APP->history->push(h);
    }

    void appendContextMenu(Menu* menu) override {
        menu->addChild(new MenuSeparator);
        const bool clockedPatched = isConnectedToClocked();
        menu->addChild(createMenuItem(
            clockedPatched
                ? "Auto-patch Clocked (already connected)"
                : "Auto-patch Clocked (BAR ÷4, STEP ×4)",
            "",
            [this]() { autopatchClocked(); },
            /* disabled */ clockedPatched
        ));
        const bool trackerPatched = isConnectedToTracker();
        menu->addChild(createMenuItem(
            trackerPatched
                ? "Auto-patch Tracker (already connected)"
                : "Auto-patch Tracker (beat/bar/step → BEAT/BAR/STEP)",
            "",
            [this]() { autopatchTracker(); },
            /* disabled */ trackerPatched
        ));
    }

    // UI-thread poll: walk cables from each CLK input to its source
    // port, and if the source is a Clocked module, copy its ratio
    // ParamQuantity's display string into the shared state so the JS
    // side can label each LED. The audio thread mustn't do this — the
    // cable graph lives in the widget layer, and getDisplayValueString
    // allocates. We already run at 60Hz here; a 6Hz throttle is plenty
    // since the labels only change when the user turns a Clocked knob.
    int labelPollCounter = 0;
    void step() override {
        ModuleWidget::step();
        if (!module) return;
        if (++labelPollCounter < 10) return;
        labelPollCounter = 0;
        refreshRatioLabels();
    }

    // Format a Clocked-style ratio (positive = multiply, negative =
    // divide) as a compact display string with the standard multiply /
    // divide glyphs. Integer values print without a decimal; fractional
    // values (1.5, 2.5) keep a single decimal. UTF-8 output: "×" is
    // 0xC3 0x97, "÷" is 0xC3 0xB7.
    static std::string formatRatio(float r) {
        if (!std::isfinite(r) || r == 0.f) return "";
        const bool div = r < 0.f;
        const float mag = div ? -r : r;
        char num[16];
        if (mag == std::floor(mag))
            std::snprintf(num, sizeof(num), "%d", (int)std::lround(mag));
        else
            std::snprintf(num, sizeof(num), "%.1f", mag);
        std::string out = div ? "\xc3\xb7" : "\xc3\x97";
        out += num;
        return out;
    }

    void refreshRatioLabels() {
        // Cache Clocked's + Tracker's Model* the first time we find them.
        // Plugin models live for the process lifetime, so this is safe.
        static rack::plugin::Model* clockedModel = nullptr;
        static rack::plugin::Model* trackerModel = nullptr;
        if (!clockedModel) {
            if (auto* p = rack::plugin::getPlugin("ImpromptuModular"))
                clockedModel = p->getModel("Clocked");
        }
        if (!trackerModel) {
            if (auto* p = rack::plugin::getPlugin("EnigmaCurry"))
                trackerModel = p->getModel("Tracker");
        }

        // Zero every slot up front — any input that isn't cabled this
        // tick shows no label, even if it did before.
        std::memset(g_webbridge_state.clockRatioLabels, 0,
                    sizeof(g_webbridge_state.clockRatioLabels));

        // beatsPerBar defaults to 4 (common time) until we prove BAR_IN
        // is cabled to a divisor output that says otherwise.
        uint32_t derivedBpb = 4;

        auto writeLabel = [](int i, const std::string& s) {
            const size_t cap = WB_RATIO_LABEL_BYTES - 1;  // leave 1 for null
            const size_t n = std::min(s.size(), cap);
            std::memcpy(g_webbridge_state.clockRatioLabels[i], s.data(), n);
            g_webbridge_state.clockRatioLabels[i][n] = 0;
        };

        for (int i = 0; i < 3; ++i) {
            // Find the PortWidget for this input port id (getInputs()
            // isn't ordered by portId, so scan for a match). Index 0=beat,
            // 1=bar, 2=step — matches the InputIds enum.
            PortWidget* myPort = nullptr;
            for (PortWidget* p : getInputs()) {
                if (p->portId == EnigmaCurryWebBridge::BEAT_IN + i) {
                    myPort = p; break;
                }
            }
            if (!myPort) continue;
            auto cables = APP->scene->rack->getCablesOnPort(myPort);
            if (cables.empty()) continue;
            CableWidget* cw = *cables.begin();  // inputs take at most one cable
            // Source is the port on the OTHER end of the cable.
            PortWidget* src = (cw->outputPort == myPort) ? cw->inputPort
                                                        : cw->outputPort;
            if (!src || !src->module) continue;
            const int srcId = src->portId;

            // -- Clocked-specific path -------------------------------------
            // Clocked's ratio labels reflect the LIVE knob position, not a
            // static port name, so we read its ratio ParamQuantity directly.
            // Also derives beatsPerBar from the source connected to BAR_IN
            // (i==1) when it's a Clocked ÷N ratio.
            if (clockedModel && src->module->model == clockedModel) {
                if (srcId < 0 || srcId > 3) continue;
                if (srcId == 0) {
                    writeLabel(i, "\xc3\x97" "1");  // "×1" in UTF-8
                    continue;
                }
                // Ratio param index matches the sub-clock output index
                // (both start at 1). See the clocked_ids namespace above
                // for the derivation. RatioParam::getDisplayValue() returns
                // the actual multiplier (positive for x, negative for ÷),
                // so we format from that directly rather than using
                // getDisplayValueString() which appends Clocked's ugly
                // " (÷)" unit suffix.
                const int paramIdx = srcId;
                if (paramIdx >= (int)src->module->paramQuantities.size()) continue;
                auto* pq = src->module->paramQuantities[paramIdx];
                if (!pq) continue;
                const float ratio = pq->getDisplayValue();
                writeLabel(i, formatRatio(ratio));

                // BAR_IN (i==1) defines the bar boundary. Only accept
                // honest divisor ratios (÷N) — ×N would make each "bar"
                // a fraction of a beat, which isn't a musically meaningful
                // bar length, so we let the default of 4 stand in that
                // case.
                if (i == 1 && ratio < 0.f) {
                    int n = (int)std::lround(-ratio);
                    if (n >= 1) derivedBpb = (uint32_t)n;
                }
                continue;
            }

            // -- Tracker-specific path -------------------------------------
            // Render Clocked-style ratio glyphs for Tracker's clock outs
            // so the two clock sources produce visually consistent labels.
            // Tracker's port IDs come from EnigmaCurryTracker's OutputIds
            // enum: OUT_L=0, OUT_R=1, OUT_BPM=2, OUT_BEAT=3, OUT_BAR=4,
            // OUT_STEP=5. Values are locked to the RPB=4 / RPM=16
            // assumption Tracker currently ships (see the comment in
            // Tracker.cpp explaining why per-pattern meter isn't wired
            // up yet).
            if (trackerModel && src->module->model == trackerModel) {
                if (srcId == 3) {           // OUT_BEAT
                    writeLabel(i, "\xc3\x97" "1");   // "×1"
                } else if (srcId == 4) {    // OUT_BAR
                    writeLabel(i, "\xc3\xb7" "4");   // "÷4"
                    // A bar-per-4-beats pulse implies bar length 4 —
                    // matches Clocked's "BAR_IN ÷N sets beatsPerBar" idea.
                    if (i == 1) derivedBpb = 4;
                } else if (srcId == 5) {    // OUT_STEP
                    writeLabel(i, "\xc3\x97" "4");   // "×4" (rows/beat)
                }
                continue;
            }

            // -- Generic fallback ------------------------------------------
            // Any other module: use whatever the source declared via
            // configOutput(portId, name). Works for modules that name
            // their outputs concisely; longer names get truncated to
            // fit the 15-char buffer. If the module didn't set a name,
            // the label stays empty.
            rack::engine::Module* srcMod = src->module;
            if (srcId < 0 || srcId >= (int)srcMod->outputInfos.size()) continue;
            const rack::engine::PortInfo* info = srcMod->outputInfos[srcId];
            if (!info || info->name.empty()) continue;
            writeLabel(i, info->name);
        }

        g_webbridge_state.beatsPerBar = derivedBpb;
    }

    EnigmaCurryWebBridgeWidget(EnigmaCurryWebBridge* module) {
        setModule(module);
        setPanel(new BrushedMetalPanel(WB_HP));

        // Everything's on the grid — ports in col 1, labels in col 0,
        // rows 1..7. Label bg colour signals direction:
        //   outputs → black-transparent bg
        //   inputs  → red-transparent bg
        // The two BPM ports carry _out / _in suffixes to disambiguate
        // beyond the colour alone.
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(1, 1), module, EnigmaCurryWebBridge::RESET_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(2, 1), module, EnigmaCurryWebBridge::RUN_OUT));
        addOutput(createOutputCentered<PJ301MPort>(
            webBridgeGrid.loc(3, 1), module, EnigmaCurryWebBridge::BPM_OUT));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(4, 1), module, EnigmaCurryWebBridge::BPM_IN));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(5, 1), module, EnigmaCurryWebBridge::BEAT_IN));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(6, 1), module, EnigmaCurryWebBridge::BAR_IN));
        addInput(createInputCentered<PJ301MPort>(
            webBridgeGrid.loc(7, 1), module, EnigmaCurryWebBridge::STEP_IN));

        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(WB_HP);
        overlay->addText("WebBridge", 14, Vec(mm2px(WB_HP * HP_UNIT / 2), 14),
                         WHITE, CLEAR, MANROPE);
        overlay->addText("reset",  10, webBridgeGrid.loc(1, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("run",    10, webBridgeGrid.loc(2, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("bpm_out", 10, webBridgeGrid.loc(3, 0),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("bpm_in",  10, webBridgeGrid.loc(4, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("beat",   10, webBridgeGrid.loc(5, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("bar",    10, webBridgeGrid.loc(6, 0),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("step", 10, webBridgeGrid.loc(7, 0),
                         WHITE, RED_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }
};

Model* modelEnigmaCurryWebBridge =
    createModel<EnigmaCurryWebBridge, EnigmaCurryWebBridgeWidget>("WebBridge");
