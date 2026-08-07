/*
 * Tracker — MPTM (OpenMPT) tracker file player.
 *
 * Copyright (C) 2026 EnigmaCurry — GPL-3.0-or-later
 *
 * Loads an MPTM/IT/XM/MOD/… tracker file (whatever libopenmpt can parse)
 * via a right-click menu and plays it as stereo L/R audio. The module
 * file is the clock source of truth — it carries tempo (fractional),
 * speed (ticks/row), and per-pattern rows-per-beat + rows-per-measure.
 * Instantaneous BPM, beat, and bar clock outputs are the natural next
 * step (headroom left in the 10 HP panel for them).
 *
 * Run is level-sensitive at ±10V (playback runs while ≥1V, holds
 * position while <1V). Reset is a trigger that seeks to song start.
 *
 * File persistence: only the absolute path is stored in the patch. Loading
 * on a different machine or in the web build silently falls back to "no
 * file" — embed-bytes-in-patch is a follow-up if portability matters.
 *
 * Decoding is done in a small ring buffer refilled from libopenmpt's
 * read() so we're not paying per-sample overhead. mod ownership is
 * guarded by a mutex because file load happens on the UI thread while
 * process() runs on the audio thread.
 */

#include "components.hpp"
#include "plugin.hpp"

// rack.hpp does not re-export the free getPlugin(slug) lookup used by the
// autopatch context-menu action — pull the header in explicitly (matches
// Mixer8.cpp and WebBridge.cpp).
#include <plugin.hpp>

#include "libopenmpt/libopenmpt.hpp"

#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>

// Cardinal's cross-platform file browser (native + wasm). Declared in
// Cardinal/include/common.hpp — the wasm build stubs osdialog.h so the
// standard Rack osdialog_file() is unavailable there.
extern void async_dialog_filebrowser(bool saving, const char* defaultName,
                                     const char* startDir, const char* title,
                                     std::function<void(char* path)> action);

struct EnigmaCurryTracker : Module {
    enum ParamIds { NUM_PARAMS };
    enum InputIds  { RUN_IN, RESET_IN, NUM_INPUTS };
    enum OutputIds {
        OUT_L, OUT_R,
        OUT_BPM,   // 1V/oct around 120 BPM (Impromptu Clocked convention)
        OUT_BEAT,  // 10V, ~1ms trigger fired on rows where row % RPB == 0
        OUT_BAR,   // 10V, ~1ms trigger fired on rows where row % RPM == 0
        NUM_OUTPUTS
    };
    enum LightIds  { NUM_LIGHTS };

    // Path to the currently loaded module file. Empty = nothing loaded.
    // Persisted verbatim via dataToJson.
    std::string modulePath;

    // Decoder owned by this module. Constructed on file load, destroyed
    // on next load or module destruction. Guarded by modMutex because
    // load happens on the UI thread while read()/set_position happen on
    // the audio thread.
    std::unique_ptr<openmpt::module> mod;
    std::mutex modMutex;

    dsp::SchmittTrigger resetTrigger;

    // Small decode ring — libopenmpt reads faster in blocks than one sample
    // at a time. RING_SIZE=128 means ~375 read() calls/second at 48kHz
    // rather than 48000. Balances latency (~2.7 ms) against per-call
    // overhead. Refilled on demand from process().
    static constexpr int RING_SIZE = 128;
    float ringL[RING_SIZE] = {0};
    float ringR[RING_SIZE] = {0};
    int ringHead = 0;   // next sample to consume
    int ringFill = 0;   // valid samples remaining ahead of ringHead

    // Row-change tracking for beat/bar triggers. -1 = uninitialised, fires
    // triggers on the very first observed row (row 0 is usually a beat AND
    // bar boundary, so the downstream sequencer starts synced).
    int lastRow     = -1;
    int lastPattern = -1;
    dsp::PulseGenerator beatPulse, barPulse;
    // ~1ms — long enough for downstream triggers to catch, short enough
    // not to smear back-to-back beat rows in fast tempos.
    static constexpr float TRIG_DUR = 1e-3f;

    EnigmaCurryTracker() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configInput(RUN_IN, "Run gate (±10V)");
        configInput(RESET_IN, "Reset trigger");
        configOutput(OUT_L, "Left");
        configOutput(OUT_R, "Right");
        configOutput(OUT_BPM,  "BPM CV (1V/oct, ref=120)");
        configOutput(OUT_BEAT, "Beat trigger");
        configOutput(OUT_BAR,  "Bar trigger");
    }

    // Called on UI thread (menu action or dataFromJson). Parses the file
    // and installs the new decoder under modMutex. On any error, mod is
    // left unchanged and a WARN is logged; the module continues playing
    // whatever was previously loaded (or silence).
    void loadModuleFromPath(const std::string& path) {
        if (path.empty()) return;
        std::unique_ptr<openmpt::module> newMod;
        try {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.good()) {
                WARN("Tracker: cannot open %s", path.c_str());
                return;
            }
            newMod = std::make_unique<openmpt::module>(ifs);
            // Loop forever — modular patches want a continuous signal.
            newMod->set_repeat_count(-1);
        } catch (const std::exception& e) {
            WARN("Tracker: failed to load %s: %s", path.c_str(), e.what());
            return;
        }
        std::lock_guard<std::mutex> lock(modMutex);
        mod = std::move(newMod);
        modulePath = path;
        ringHead = ringFill = 0;
        lastRow = lastPattern = -1;
    }

    void process(const ProcessArgs& args) override {
        const bool running = inputs[RUN_IN].getVoltage() >= 1.f;
        const bool resetEdge =
            resetTrigger.process(inputs[RESET_IN].getVoltage(), 0.1f, 1.f);

        // try_lock so the audio thread never blocks on a file load in
        // progress. A held mutex means we emit silence for this sample.
        std::unique_lock<std::mutex> lock(modMutex, std::try_to_lock);
        float L = 0.f, R = 0.f;
        float bpmCv = 0.f;   // 0V default = 120 BPM

        if (lock.owns_lock() && mod) {
            if (resetEdge) {
                mod->set_position_order_row(0, 0);
                ringHead = ringFill = 0;
                lastRow = lastPattern = -1;
            }

            const int pat = mod->get_current_pattern();
            // Per-pattern rows-per-beat / rows-per-measure would come from
            // openmpt::module::get_pattern_rows_per_beat() / _per_measure()
            // (0.8.x API). This build vendors 0.7.9 (Emscripten 3.1.27 in
            // Cardinal's PawPaw pins doesn't meet 0.8.x's 3.1.51 floor), so
            // we fall back to the 4/4 tracker convention. Fine for MOD/XM
            // (which don't carry meter anyway) and correct for MPTM files
            // that keep the defaults. Bump libopenmpt when Cardinal's
            // Emscripten does.
            constexpr int rpb = 4;
            constexpr int rpm = 16;

            // BPM CV — emitted continuously so downstream tempo consumers
            // see the current tempo even while playback is paused. Classic
            // tracker BPM = 24 × tempo / (speed × RPB); MPTM's fractional
            // tempo mode falls out of the same formula because tempo2 is
            // already the fractional per-tick rate.
            const double tempo = mod->get_current_tempo2();
            const int speed = mod->get_current_speed();
            if (speed > 0) {
                const double bpm = 24.0 * tempo / (double)(speed * rpb);
                if (bpm > 0.0) {
                    bpmCv = (float)std::log2(bpm / 120.0);
                }
            }

            if (running) {
                if (ringFill == 0) {
                    // Refill. libopenmpt returns the number of frames it
                    // actually produced (may be < requested near EOF, but
                    // we loop repeat_count=-1 so it should always fill).
                    std::size_t got = mod->read(
                        (std::int32_t)args.sampleRate,
                        RING_SIZE, ringL, ringR);
                    ringHead = 0;
                    ringFill = (int)got;
                }
                if (ringFill > 0) {
                    // libopenmpt returns [-1, +1]; Rack audio nominal ±5V.
                    L = ringL[ringHead] * 5.f;
                    R = ringR[ringHead] * 5.f;
                    ringHead++;
                    ringFill--;
                }

                // Row-change → beat/bar triggers. Uses (row, pattern)
                // rather than sample-time accumulation so jumps, loops,
                // and tempo changes are honored automatically.
                const int row = mod->get_current_row();
                if (row != lastRow || pat != lastPattern) {
                    if (row % rpb == 0) beatPulse.trigger(TRIG_DUR);
                    if (row % rpm == 0) barPulse.trigger(TRIG_DUR);
                    lastRow = row;
                    lastPattern = pat;
                }
            }
        }

        outputs[OUT_L].setVoltage(L);
        outputs[OUT_R].setVoltage(R);
        outputs[OUT_BPM].setVoltage(bpmCv);
        // Pulse generators are audio-thread-only, safe to process outside
        // the mutex.
        outputs[OUT_BEAT].setVoltage(
            beatPulse.process(args.sampleTime) ? 10.f : 0.f);
        outputs[OUT_BAR].setVoltage(
            barPulse.process(args.sampleTime) ? 10.f : 0.f);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "modulePath",
                            json_string(modulePath.c_str()));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* pathJ = json_object_get(rootJ, "modulePath");
        if (pathJ) {
            const char* p = json_string_value(pathJ);
            if (p && *p) loadModuleFromPath(p);
        }
    }
};

#define HP 10
#define ROWS 10
#define COLUMNS 2
static panel_grid<HP, ROWS, COLUMNS> trackerGrid;

struct EnigmaCurryTrackerWidget : ModuleWidget {
    EnigmaCurryTrackerWidget(EnigmaCurryTracker* module) {
        setModule(module);
        setPanel(new BrushedMetalPanel(HP));

        // Shift port matrix ~6px right so col-0 labels have room to the
        // LEFT of their ports (matches Mixer8's convention).
        auto at = [](int r, int c) {
            return trackerGrid.loc(r, c).plus(Vec(6, 0));
        };

        // Col 0: Run gate + Reset trigger. Placed low enough that the L/R
        // outputs (pinned to HostAudio2 y-coords, ~85 and ~114 px, near
        // the top) don't visually crowd them.
        addInput(createInputCentered<PJ301MPort>(
            at(3, 0), module, EnigmaCurryTracker::RUN_IN));
        addInput(createInputCentered<PJ301MPort>(
            at(5, 0), module, EnigmaCurryTracker::RESET_IN));

        // Col 1: stereo master out, y-pinned to align with HostAudio2's
        // Left/M and Right jack CENTERS. HostAudio uses createInput
        // (top-left origin) with startY=73 and padding=29 (Cardinal
        // ModuleWidgets.hpp); PJ301M is 23.7px tall so its centers land
        // at y=73+11.85 and y=102+11.85. Same trick as Mixer8, so
        // autopatch cables run straight across.
        const float col1X = at(0, 1).x;
        const Vec outLPos = Vec(col1X, 73.f + 11.85f);
        const Vec outRPos = Vec(col1X, 102.f + 11.85f);
        addOutput(createOutputCentered<PJ301MPort>(
            outLPos, module, EnigmaCurryTracker::OUT_L));
        addOutput(createOutputCentered<PJ301MPort>(
            outRPos, module, EnigmaCurryTracker::OUT_R));

        // Col 1 clock outputs: BPM CV + Beat + Bar triggers stacked below
        // the L/R audio outs. Rows 4/6/8 give even spacing without
        // crowding the L/R block above.
        const Vec bpmPos  = at(4, 1);
        const Vec beatPos = at(6, 1);
        const Vec barPos  = at(8, 1);
        addOutput(createOutputCentered<PJ301MPort>(
            bpmPos,  module, EnigmaCurryTracker::OUT_BPM));
        addOutput(createOutputCentered<PJ301MPort>(
            beatPos, module, EnigmaCurryTracker::OUT_BEAT));
        addOutput(createOutputCentered<PJ301MPort>(
            barPos,  module, EnigmaCurryTracker::OUT_BAR));

        // Static labels — module name + jack tags. Run tag on
        // black-transparent bg, Reset on red-transparent to signal the
        // trigger nature (matches Latch/Transport reset styling). Clock
        // triggers get the red-transparent bg for the same reason.
        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(HP);
        overlay->addText("Tracker", 18, Vec(mm2px(HP * HP_UNIT / 2), 25),
                         WHITE, CLEAR, MANROPE);
        overlay->addText("RUN", 9, at(3, 0).plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("RST", 9, at(5, 0).plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("L", 10, outLPos.plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("R", 10, outRPos.plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BPM", 9, bpmPos.plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BT",  9, beatPos.plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("BR",  9, barPos.plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }

    // -------------------------------------------------------------------
    // File loader — right-click "Load MPTM file…".
    // Uses Cardinal's async browser so it works in both native and wasm
    // builds. Callback fires on dialog close; `path` is null on cancel
    // and must be free()'d otherwise.
    // -------------------------------------------------------------------
    void loadModuleFile() {
        EnigmaCurryTracker* mod = dynamic_cast<EnigmaCurryTracker*>(module);
        if (!mod) return;

        // Start dialog in the directory of the previously loaded file,
        // or the user's Rack asset dir if nothing has been loaded.
        std::string dir = mod->modulePath.empty()
            ? asset::user("")
            : system::getDirectory(mod->modulePath);

        async_dialog_filebrowser(false, NULL, dir.c_str(), "Load MPTM file",
            [mod](char* path) {
                if (!path) return;
                mod->loadModuleFromPath(std::string(path));
                free(path);
            });
    }

    // -------------------------------------------------------------------
    // Autopatch to Cardinal HostAudio2 — mirror of Mixer8's helper.
    // Reuse an existing HostAudio2 whose L/R inputs are both unpatched;
    // otherwise spawn a new one and place it flush right (our L/R outs
    // already sit on the right edge at HostAudio2-aligned y-coords).
    // -------------------------------------------------------------------
    static constexpr int HA2_IN_L = 0;
    static constexpr int HA2_IN_R = 1;

    static rack::plugin::Model* hostAudio2Model() {
        rack::plugin::Plugin* cardinal = rack::plugin::getPlugin("Cardinal");
        if (!cardinal) return nullptr;
        return cardinal->getModel("HostAudio2");
    }

    // True if EITHER OUT_L or OUT_R has a cable to a HostAudio2. Used to
    // grey out the menu item so a second click can't hijack or duplicate.
    bool isConnectedToHostAudio() {
        rack::plugin::Model* haModel = hostAudio2Model();
        if (!haModel) return false;
        const int outIds[2] = { EnigmaCurryTracker::OUT_L,
                                EnigmaCurryTracker::OUT_R };
        for (int outId : outIds) {
            PortWidget* myPort = nullptr;
            for (PortWidget* p : getOutputs()) {
                if (p->portId == outId) { myPort = p; break; }
            }
            if (!myPort) continue;
            for (CableWidget* cw : APP->scene->rack->getCablesOnPort(myPort)) {
                PortWidget* other = (cw->outputPort == myPort) ? cw->inputPort
                                                              : cw->outputPort;
                if (other && other->module && other->module->model == haModel)
                    return true;
            }
        }
        return false;
    }

    ModuleWidget* findUnpatchedHostAudio2() {
        rack::plugin::Model* haModel = hostAudio2Model();
        if (!haModel) return nullptr;
        rack::app::RackWidget* rw = APP->scene->rack;
        for (Widget* w : rw->getModuleContainer()->children) {
            ModuleWidget* mw = dynamic_cast<ModuleWidget*>(w);
            if (!mw || !mw->getModule() || mw->getModule()->model != haModel)
                continue;
            bool anyPatched = false;
            for (PortWidget* p : mw->getInputs()) {
                if (p->portId != HA2_IN_L && p->portId != HA2_IN_R) continue;
                if (!rw->getCablesOnPort(p).empty()) {
                    anyPatched = true; break;
                }
            }
            if (!anyPatched) return mw;
        }
        return nullptr;
    }

    void autopatchHostAudio() {
        if (isConnectedToHostAudio()) return;  // defensive; menu is disabled

        rack::plugin::Model* haModel = hostAudio2Model();
        if (!haModel) {
            WARN("Tracker autopatch: Cardinal HostAudio2 model not found");
            return;
        }

        rack::app::RackWidget* rw = APP->scene->rack;
        ModuleWidget* haWidget = findUnpatchedHostAudio2();
        history::ModuleAdd* moduleAddAction = nullptr;

        if (!haWidget) {
            engine::Module* haModule = haModel->createModule();
            APP->engine->addModule(haModule);
            haWidget = haModel->createModuleWidget(haModule);
            if (!haWidget) {
                WARN("Tracker autopatch: createModuleWidget returned null");
                return;
            }

            // Placement policy mirrors Mixer8/WebBridge: right-flush first
            // (our L/R sit on the right edge, HostAudio2's L/R inputs sit
            // on its left edge — straight cable run). Fall back to
            // left-flush, then shove-mode in a fixed rack, then overflow.
            const Vec preferredRight = box.pos + Vec(box.size.x, 0);
            const Vec preferredLeft  = box.pos - Vec(haWidget->box.size.x, 0);
            bool placed =
                rw->requestModulePos(haWidget, preferredRight) ||
                rw->requestModulePos(haWidget, preferredLeft);

            if (!placed && rack::settings::rackspaceFixed) {
                std::vector<std::pair<Widget*, Vec>> snap;
                for (Widget* w : rw->getModuleContainer()->children)
                    snap.push_back(std::make_pair(w, w->box.pos));
                const rack::math::Rect region = rack::app::getFiniteRackBox();
                const Vec shoveTargets[2] = {
                    Vec(region.pos.x + region.size.x - haWidget->box.size.x,
                        box.pos.y),
                    Vec(region.pos.x, box.pos.y),
                };
                for (int t = 0; t < 2; ++t) {
                    for (size_t i = 0; i < snap.size(); ++i)
                        snap[i].first->setPosition(snap[i].second);
                    rw->setModulePosForce(haWidget, shoveTargets[t]);
                    bool allInside = region.contains(haWidget->box);
                    if (allInside) {
                        for (Widget* w : rw->getModuleContainer()->children) {
                            if (!region.contains(w->box)) {
                                allInside = false; break;
                            }
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
                    outsidePos.x = rack::app::getFiniteRackBox().getRight()
                                 + RACK_GRID_WIDTH;
                    for (size_t i = 0;
                         i < rack::settings::rackspaceRegions.size(); ++i) {
                        int offHP, offRow, wHP, hRow;
                        if (!rack::settings::resolveRegionBounds(
                                (int)i, offHP, offRow, wHP, hRow))
                            continue;
                        rack::math::Rect regionBox;
                        regionBox.pos = RACK_OFFSET
                                      + Vec(offHP * RACK_GRID_WIDTH,
                                            offRow * RACK_GRID_HEIGHT);
                        regionBox.size = Vec(wHP * RACK_GRID_WIDTH,
                                             hRow * RACK_GRID_HEIGHT);
                        rack::math::Rect proposed(outsidePos, haWidget->box.size);
                        if (regionBox.intersects(proposed))
                            outsidePos.x = regionBox.getRight()
                                         + RACK_GRID_WIDTH;
                    }
                }
                rw->setModulePosForce(haWidget, outsidePos);
            }

            rw->addModule(haWidget);

            moduleAddAction = new history::ModuleAdd;
            moduleAddAction->name = "auto-patch Host Audio to Tracker";
            moduleAddAction->setModule(haWidget);
        }

        auto findInput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getInputs())
                if (p->portId == portId) return p;
            return nullptr;
        };
        auto findOutput = [](ModuleWidget* mw, int portId) -> PortWidget* {
            for (PortWidget* p : mw->getOutputs())
                if (p->portId == portId) return p;
            return nullptr;
        };
        auto connect = [](PortWidget* outPort, PortWidget* inPort)
                       -> CableWidget* {
            if (!outPort || !inPort) return nullptr;
            CableWidget* cw = new CableWidget();
            cw->color = APP->scene->rack->getNextCableColor();
            cw->outputPort = outPort;
            cw->inputPort  = inPort;
            cw->updateCable();
            APP->scene->rack->addCable(cw);
            return cw;
        };

        CableWidget* cL = connect(findOutput(this, EnigmaCurryTracker::OUT_L),
                                  findInput (haWidget, HA2_IN_L));
        CableWidget* cR = connect(findOutput(this, EnigmaCurryTracker::OUT_R),
                                  findInput (haWidget, HA2_IN_R));

        if (moduleAddAction) {
            APP->history->push(moduleAddAction);
        } else {
            history::ComplexAction* h = new history::ComplexAction;
            h->name = "auto-patch Host Audio to Tracker";
            for (CableWidget* cw : {cL, cR}) {
                if (!cw) continue;
                history::CableAdd* ca = new history::CableAdd;
                ca->setCable(cw);
                h->push(ca);
            }
            if (!h->isEmpty())
                APP->history->push(h);
            else
                delete h;
        }
    }

    void appendContextMenu(Menu* menu) override {
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuItem(
            "Load MPTM file…", "",
            [this]() { loadModuleFile(); }
        ));

        menu->addChild(new MenuSeparator);
        const bool alreadyPatched = isConnectedToHostAudio();
        menu->addChild(createMenuItem(
            alreadyPatched
                ? "Auto-patch Host Audio (already connected)"
                : "Auto-patch Host Audio",
            "",
            [this]() { autopatchHostAudio(); },
            /* disabled */ alreadyPatched
        ));
    }
};

Model* modelEnigmaCurryTracker =
    createModel<EnigmaCurryTracker, EnigmaCurryTrackerWidget>("Tracker");
