/*
 * Negative Harmony
 * Copyright (C) 2023 EnigmaCurry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or any later version. At your discretion, you may alternatively
 * redistribute it and/or modify it under the terms of the MIT License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License and/or MIT License for more details.
 *
 * For a full copy of the GNU General Public License see the /LICENSE file.
 * For a full copy of the MIT License see the /LICENSE.MIT file.
 */

#include "components.hpp"
#include "plugin.hpp"
#include <string>
#include "scale.hpp"

struct NegativeHarmony : Module {
  enum ParamIds { TONIC, NUM_PARAMS };
  enum InputIds { TONIC_IN, NOTE_IN, GATE_IN, VELOCITY_IN, NUM_INPUTS };
  enum OutputIds { NOTE_OUT, GATE_OUT, VELOCITY_OUT, NUM_OUTPUTS };
  enum LightIds { NUM_LIGHTS };
  chromaticNote tonic = middleC();
  
  NegativeHarmony() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(TONIC, -5, 5, 0, "Tonic Axis");
    configInput(TONIC_IN, "Tonic CV");
    configInput(NOTE_IN, "NOTE_IN");
    configOutput(NOTE_OUT, "NOTE_OUT");
    configInput(GATE_IN, "GATE_IN");
    configOutput(GATE_OUT, "GATE_OUT");
    configInput(VELOCITY_IN, "VELOCITY_IN");
    configOutput(VELOCITY_OUT, "VELOCITY_OUT");
  }

  void process(const ProcessArgs &args) override {
    float tonic_volts = 0.0;
    if (inputs[TONIC_IN].isConnected()) {
      tonic_volts = std::min(5.0f, std::max(-5.0f, inputs[TONIC_IN].getNormalVoltage(0.0)));
    } else {
      tonic_volts = params[TONIC].getValue();
    }
    tonic = chromaticNoteFromVoltage(tonic_volts);
  }
};

#define HP 6
#define ROWS 14
#define COLUMNS 11
panel_grid<HP, ROWS, COLUMNS> negativeHarmonyGrid;
Vec displayLoc = negativeHarmonyGrid.loc(1,7).minus(Vec(0,4));

struct NegativeHarmonyDisplay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  NegativeHarmony *module;

  NegativeHarmonyDisplay(int hp_width) : DynamicOverlay(hp_width) {}

  void draw(const DrawArgs &args) override {
    Vec tonicTextLoc = displayLoc.plus(Vec(0,24));
    if (module) {
      DynamicOverlay::clear();
      addText(chromaticNoteToString(module->tonic), 10, tonicTextLoc,
              RED, CLEAR, DSEG);
      DynamicOverlay::draw(args);
    } else {
      // Draw example display for module browser:
      DynamicOverlay::clear();
      addText("C4", 12, tonicTextLoc,
              RED, CLEAR, DSEG);
      DynamicOverlay::draw(args);
    }
  }
};


struct NegativeHarmonyWidget : ModuleWidget {
  NegativeHarmonyWidget(NegativeHarmony *module) {
    setModule(module);
    setPanel(
             APP->window->loadSvg(asset::plugin(pluginInstance, "res/6hp.svg")));

    Vec tonicInLoc = negativeHarmonyGrid.loc(4,2);
    Vec tonicKnobLoc = negativeHarmonyGrid.loc(4,7);
    Vec noteInLoc = negativeHarmonyGrid.loc(6, 2);
    Vec noteOutLoc = negativeHarmonyGrid.loc(6, 7);

    Vec gateInLoc = negativeHarmonyGrid.loc(8, 2);
    Vec gateOutLoc = negativeHarmonyGrid.loc(8, 7);

    Vec velocityInLoc = negativeHarmonyGrid.loc(10, 2);
    Vec velocityOutLoc = negativeHarmonyGrid.loc(10, 7);

    addInput(createInputCentered<PJ301MPort>(tonicInLoc, module, NegativeHarmony::TONIC_IN));
    addInput(createInputCentered<PJ301MPort>(noteInLoc, module, NegativeHarmony::NOTE_IN));
    addInput(createInputCentered<PJ301MPort>(gateInLoc, module, NegativeHarmony::GATE_IN));
    addInput(createInputCentered<PJ301MPort>(velocityInLoc, module, NegativeHarmony::VELOCITY_IN));
    addOutput(createOutputCentered<PJ301MPort>(noteOutLoc, module, NegativeHarmony::NOTE_OUT));
    addOutput(createOutputCentered<PJ301MPort>(gateOutLoc, module, NegativeHarmony::GATE_OUT));
    addOutput(createOutputCentered<PJ301MPort>(velocityOutLoc, module, NegativeHarmony::VELOCITY_OUT));
    addParam(
             createParamCentered<RoundBlackKnob>(tonicKnobLoc, module, NegativeHarmony::TONIC));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Negative", 18, Vec(mm2px(HP * HP_UNIT / 2), 13), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("Harmony", 18, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("AXIS", 13, tonicInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("AXIS", 13, tonicKnobLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("NOTE", 13, noteInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("GATE", 13, gateInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("VEL", 13, velocityInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("NOTE", 13, noteOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("GATE", 13, gateOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("VEL", 13, velocityOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);

      buffer->addChild(overlay);
      addChild(buffer);
      overlay->addBox(displayLoc.minus(Vec(18,8)),
                      Vec(36,52),
                      BLACK_TRANSPARENT, 5);
    }
    // Draw dynamic tonic display
    {
      NegativeHarmonyDisplay *overlay = new NegativeHarmonyDisplay(HP);
      overlay->module = module;
      addChild(overlay);
    }

  }
};



Model *modelNegativeHarmony = createModel<NegativeHarmony, NegativeHarmonyWidget>("NegativeHarmony");
