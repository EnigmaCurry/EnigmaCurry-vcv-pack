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
  enum ParamIds { AXIS, NUM_PARAMS };
  enum InputIds { AXIS_IN, NOTE_IN, GATE_IN, VELOCITY_IN, NUM_INPUTS };
  enum OutputIds { NOTE_OUT, GATE_OUT, VELOCITY_OUT, NUM_OUTPUTS };
  enum LightIds { NUM_LIGHTS };
  chromaticNote axis = middleC();

  NegativeHarmony() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(AXIS, -5, 5, 0, "Axis");
    configInput(AXIS_IN, "Axis CV");
    configInput(NOTE_IN, "NOTE_IN");
    configOutput(NOTE_OUT, "NOTE_OUT");
    configInput(GATE_IN, "GATE_IN");
    configOutput(GATE_OUT, "GATE_OUT");
    configInput(VELOCITY_IN, "VELOCITY_IN");
    configOutput(VELOCITY_OUT, "VELOCITY_OUT");
  }

  void process(const ProcessArgs &args) override {
    float axis_volts = 0.0;
    int channels = inputs[NOTE_IN].getChannels();
    if (inputs[AXIS_IN].isConnected()) {
      axis_volts = std::min(5.0f, std::max(-5.0f, inputs[AXIS_IN].getNormalVoltage(0.)));
    } else {
      axis_volts = params[AXIS].getValue();
    }
    axis = chromaticNoteFromVoltage(axis_volts);
    for (int c = 0; c < channels; c++) {
      int distance = axis.midi_note - chromaticNoteFromVoltage(inputs[NOTE_IN].getPolyVoltage(c)).midi_note;
      chromaticNote mirroredNote = chromaticNoteFromMidiNote(axis.midi_note + distance);
      outputs[NOTE_OUT].setVoltage(calculateChromaticNoteVoltage(mirroredNote),c);
      // passthrough gate and velocity:
      outputs[GATE_OUT].setVoltage(inputs[GATE_IN].getNormalVoltage(0.,c),c);
      outputs[VELOCITY_OUT].setVoltage(inputs[VELOCITY_IN].getNormalVoltage(0.,c),c);
    }
    outputs[NOTE_OUT].setChannels(channels);
    outputs[GATE_OUT].setChannels(channels);
    outputs[VELOCITY_OUT].setChannels(channels);
  }
};

#define HP 6
#define ROWS 14
#define COLUMNS 11
panel_grid<HP, ROWS, COLUMNS> negativeHarmonyGrid;
Vec displayLoc = negativeHarmonyGrid.loc(1,3).minus(Vec(0,4));
struct NegativeHarmonyDisplay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  NegativeHarmony *module;

  NegativeHarmonyDisplay(int hp_width) : DynamicOverlay(hp_width) {}

  void draw(const DrawArgs &args) override {
    Vec axisTextLoc = displayLoc.plus(Vec(15,24));
    if (module) {
      DynamicOverlay::clear();
      addText(chromaticNoteToString(module->axis), 15, axisTextLoc,
              RED, CLEAR, DSEG);
      DynamicOverlay::draw(args);
    } else {
      // Draw example display for module browser:
      DynamicOverlay::clear();
      addText("C4", 12, axisTextLoc,
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

    Vec axisInLoc = negativeHarmonyGrid.loc(4,2);
    Vec axisKnobLoc = negativeHarmonyGrid.loc(4,8);
    Vec noteInLoc = negativeHarmonyGrid.loc(6, 2);
    Vec noteOutLoc = negativeHarmonyGrid.loc(6, 8);

    Vec gateInLoc = negativeHarmonyGrid.loc(8, 2);
    Vec gateOutLoc = negativeHarmonyGrid.loc(8, 8);

    Vec velocityInLoc = negativeHarmonyGrid.loc(10, 2);
    Vec velocityOutLoc = negativeHarmonyGrid.loc(10, 8);

    addInput(createInputCentered<PJ301MPort>(axisInLoc, module, NegativeHarmony::AXIS_IN));
    addInput(createInputCentered<PJ301MPort>(noteInLoc, module, NegativeHarmony::NOTE_IN));
    addInput(createInputCentered<PJ301MPort>(gateInLoc, module, NegativeHarmony::GATE_IN));
    addInput(createInputCentered<PJ301MPort>(velocityInLoc, module, NegativeHarmony::VELOCITY_IN));
    addOutput(createOutputCentered<PJ301MPort>(noteOutLoc, module, NegativeHarmony::NOTE_OUT));
    addOutput(createOutputCentered<PJ301MPort>(gateOutLoc, module, NegativeHarmony::GATE_OUT));
    addOutput(createOutputCentered<PJ301MPort>(velocityOutLoc, module, NegativeHarmony::VELOCITY_OUT));
    addParam(
             createParamCentered<RoundBlackKnob>(axisKnobLoc, module, NegativeHarmony::AXIS));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Negative", 18, Vec(mm2px(HP * HP_UNIT / 2), 13), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("Harmony", 18, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("AXIS", 13, axisInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("AXIS", 13, axisKnobLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("V/OCT", 13, noteInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("GATE", 13, gateInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("VEL", 13, velocityInLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("V/OCT", 13, noteOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("GATE", 13, gateOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("VEL", 13, velocityOutLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);

      buffer->addChild(overlay);
      addChild(buffer);
      overlay->addBox(displayLoc.minus(Vec(18,8)),
                      Vec(70,52),
                      BLACK_TRANSPARENT, 5);
    }
    // Draw dynamic axis display
    {
      NegativeHarmonyDisplay *overlay = new NegativeHarmonyDisplay(HP);
      overlay->module = module;
      addChild(overlay);
    }

  }
};



Model *modelNegativeHarmony = createModel<NegativeHarmony, NegativeHarmonyWidget>("NegativeHarmony");
