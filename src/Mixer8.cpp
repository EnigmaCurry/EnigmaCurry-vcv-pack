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

// rack.hpp deliberately does NOT re-export the free `getPlugin(slug)`
// lookup, so pull the header in explicitly for the autopatch
// context-menu action (matches WebBridge.cpp).
#include <plugin.hpp>

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
        setPanel(new BrushedMetalPanel(HP));

        // Shift the whole port matrix ~6px right so col-0 labels have
        // room to the LEFT of their ports without hugging the panel edge.
        // Col-2 (master L/R) still has ample right-side padding after this.
        auto at = [](int r, int c) {
            return mixer8Grid.loc(r, c).plus(Vec(6, 0));
        };

        // Col 0 rows 1..8: 8 channel inputs.
        for (int ch = 0; ch < 8; ++ch) {
            addInput(createInputCentered<PJ301MPort>(
                at(1 + ch, 0), module,
                EnigmaCurryMixer8::IN0 + ch));
        }

        // Col 1 rows 1..8: two lane-grouped aux buses.
        // Lane A (rows 1..4): SEND L, SEND R, RET L, RET R
        // Lane B (rows 5..8): SEND L, SEND R, RET L, RET R
        addOutput(createOutputCentered<PJ301MPort>(
            at(1, 1), module, EnigmaCurryMixer8::SEND_AL));
        addOutput(createOutputCentered<PJ301MPort>(
            at(2, 1), module, EnigmaCurryMixer8::SEND_AR));
        addInput(createInputCentered<PJ301MPort>(
            at(3, 1), module, EnigmaCurryMixer8::RET_AL));
        addInput(createInputCentered<PJ301MPort>(
            at(4, 1), module, EnigmaCurryMixer8::RET_AR));
        addOutput(createOutputCentered<PJ301MPort>(
            at(5, 1), module, EnigmaCurryMixer8::SEND_BL));
        addOutput(createOutputCentered<PJ301MPort>(
            at(6, 1), module, EnigmaCurryMixer8::SEND_BR));
        addInput(createInputCentered<PJ301MPort>(
            at(7, 1), module, EnigmaCurryMixer8::RET_BL));
        addInput(createInputCentered<PJ301MPort>(
            at(8, 1), module, EnigmaCurryMixer8::RET_BR));

        // Col 2: stereo master out, y-pinned to align with Host Audio's
        // Left/M and Right jack CENTERS. Host Audio uses createInput (top-
        // left origin) at startY=73 with padding=29 (Cardinal
        // ModuleWidgets.hpp), so its jack centers land at y=73+11.85 and
        // 102+11.85 (PJ301M is 23.7 px tall). Using createOutputCentered
        // here means we point at those centers directly.
        const float col2X = at(0, 2).x;
        const Vec outLPos = Vec(col2X, 73.f + 11.85f);
        const Vec outRPos = Vec(col2X, 102.f + 11.85f);
        addOutput(createOutputCentered<PJ301MPort>(
            outLPos, module, EnigmaCurryMixer8::OUT_L));
        addOutput(createOutputCentered<PJ301MPort>(
            outRPos, module, EnigmaCurryMixer8::OUT_R));

        // Static labels: module name + tiny channel numbers + aux tokens
        // + master L/R marks. Sends use output-black bg, returns use
        // input-red bg so the eye distinguishes them without decoding
        // the letters.
        FramebufferWidget* buffer = new FramebufferWidget();
        DynamicOverlay* overlay = new DynamicOverlay(HP);
        overlay->addText("Mixer8", 20, Vec(mm2px(HP * HP_UNIT / 2), 25),
                         WHITE, CLEAR, MANROPE);
        for (int ch = 0; ch < 8; ++ch) {
            char label[4];
            std::snprintf(label, sizeof(label), "%d", ch + 1);
            overlay->addText(label, 10,
                             at(1 + ch, 0).plus(Vec(-20, 3)),
                             WHITE, RED_TRANSPARENT);
        }
        // Lane A: SL,SR (send=black), RL,RR (return=red)
        overlay->addText("AL", 9, at(1, 1).plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("AR", 9, at(2, 1).plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("AL", 9, at(3, 1).plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("AR", 9, at(4, 1).plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("BL", 9, at(5, 1).plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BR", 9, at(6, 1).plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("BL", 9, at(7, 1).plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("BR", 9, at(8, 1).plus(Vec(-20, 3)),
                         WHITE, RED_TRANSPARENT);
        overlay->addText("L", 10, outLPos.plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        overlay->addText("R", 10, outRPos.plus(Vec(-20, 3)),
                         WHITE, BLACK_TRANSPARENT);
        buffer->addChild(overlay);
        addChild(buffer);
    }

    // -------------------------------------------------------------------
    // Autopatch: wire Mixer8's OUT_L/OUT_R to a Cardinal HostAudio2's
    // Left/M + Right inputs. Reuse an existing HostAudio2 in the rack if
    // its L/R inputs are both unpatched; otherwise spawn a new one and
    // place it flush right (Mixer8's L/R outputs already sit at the same
    // y-coords as HostAudio2's inputs, see the outLPos/outRPos comment
    // above — right-flush lands them cable-adjacent).
    //
    // HostAudio2 port ids come from HostAudio<numIO>::config, which
    // assigns numIO inputs and numIO outputs starting at 0; the widget
    // draws them as "Left/M" and "Right" in that order.
    // -------------------------------------------------------------------
    static constexpr int HA2_IN_L = 0;
    static constexpr int HA2_IN_R = 1;

    static rack::plugin::Model* hostAudio2Model() {
        rack::plugin::Plugin* cardinal = rack::plugin::getPlugin("Cardinal");
        if (!cardinal) return nullptr;
        return cardinal->getModel("HostAudio2");
    }

    // True if EITHER of Mixer8's OUT_L / OUT_R has a cable to a
    // HostAudio2. Used to grey out the menu item — clicking again
    // would either duplicate the pair or hijack the existing one.
    bool isConnectedToHostAudio() {
        rack::plugin::Model* haModel = hostAudio2Model();
        if (!haModel) return false;
        const int outIds[2] = { EnigmaCurryMixer8::OUT_L,
                                EnigmaCurryMixer8::OUT_R };
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

    // Find an existing HostAudio2 with BOTH IN_L and IN_R unpatched. If
    // one is present we reuse it instead of spawning a duplicate — the
    // user's rule: "ok to just hook up to an existing Host Audio, as
    // long as its not already hooked up".
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
            WARN("Mixer8 autopatch: Cardinal HostAudio2 model not found");
            return;
        }

        rack::app::RackWidget* rw = APP->scene->rack;
        ModuleWidget* haWidget = findUnpatchedHostAudio2();
        history::ModuleAdd* moduleAddAction = nullptr;

        if (!haWidget) {
            // Spawn a fresh HostAudio2. Placement policy mirrors
            // WebBridge::autopatchClocked: prefer right-flush of Mixer8
            // (our L/R sit on the right edge, HostAudio2's L/R inputs
            // sit on its left edge — cables run straight across). Fall
            // back to left-flush, then shove-mode in a fixed rack, then
            // last-resort overflow past any defined region.
            engine::Module* haModule = haModel->createModule();
            APP->engine->addModule(haModule);
            haWidget = haModel->createModuleWidget(haModule);
            if (!haWidget) {
                WARN("Mixer8 autopatch: createModuleWidget returned null");
                return;
            }

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
            moduleAddAction->name = "auto-patch Host Audio to Mixer8";
            moduleAddAction->setModule(haWidget);
        }

        // Wire cables. Same helper shape as WebBridge autopatchClocked.
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

        CableWidget* cL = connect(findOutput(this, EnigmaCurryMixer8::OUT_L),
                                  findInput (haWidget, HA2_IN_L));
        CableWidget* cR = connect(findOutput(this, EnigmaCurryMixer8::OUT_R),
                                  findInput (haWidget, HA2_IN_R));

        // History. When we spawned a new HostAudio2 the ModuleAdd alone
        // is enough — Rack removes cascading cables on undo. When we
        // REUSED an existing HostAudio2, no ModuleAdd occurred, so push
        // the cable adds explicitly so undo tears them down.
        if (moduleAddAction) {
            APP->history->push(moduleAddAction);
        } else {
            history::ComplexAction* h = new history::ComplexAction;
            h->name = "auto-patch Host Audio to Mixer8";
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

Model* modelEnigmaCurryMixer8 =
    createModel<EnigmaCurryMixer8, EnigmaCurryMixer8Widget>("Mixer8");
