/*
 * Trigger selectable attenuator / voltage source
 * Copyright (C) 2022 EnigmaCurry
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

struct Range : Module {
  enum ParamIds { TAP_RANGE, NUM_PARAMS };
  enum InputIds { RESET, IN_1, IN_2, IN_3, IN_4, IN_5, IN_6, IN_7, IN_8, IN_9, IN_10, IN_11, IN_12, NUM_INPUTS };
  enum OutputIds { OUT, NUM_OUTPUTS };
  enum LightIds { TAP_RANGE_LIGHT, NUM_LIGHTS };
  enum RangeModeIds { TEN_VOLTS, ONE_VOLT_PER_OCTAVE, NUM_RANGE_MODES};
  dsp::SchmittTrigger tapRangeTrigger, resetTrigger, trigger[12][16];
  dsp::PulseGenerator rangeLightPulse;

  int currentRangeMode = TEN_VOLTS;
  int triggerLabelFontSize = 12;
  std::string triggerLabels[12] = {""};
  float outVolts[16] = {0};

  Range() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(TAP_RANGE, 0.f, 1.f, 0.f, "Tap to change range");
    configInput(RESET, "Reset");
    configInput(IN_1, "Trigger 1");
    configInput(IN_2, "Trigger 2");
    configInput(IN_3, "Trigger 3");
    configInput(IN_4, "Trigger 4");
    configInput(IN_5, "Trigger 5");
    configInput(IN_6, "Trigger 6");
    configInput(IN_7, "Trigger 7");
    configInput(IN_8, "Trigger 8");
    configInput(IN_9, "Trigger 9");
    configInput(IN_10, "Trigger 10");
    configInput(IN_11, "Trigger 11");
    configInput(IN_12, "Trigger 12");
    configOutput(OUT, "OUT");
    reset();
  }

  void process(const ProcessArgs &args) override {
    int channels = std::max({1, inputs[IN_1].getChannels(),inputs[IN_2].getChannels(),inputs[IN_3].getChannels(),inputs[IN_4].getChannels(),inputs[IN_5].getChannels(),inputs[IN_6].getChannels(),inputs[IN_7].getChannels(),inputs[IN_8].getChannels(),inputs[IN_9].getChannels(),inputs[IN_10].getChannels(),inputs[IN_11].getChannels(),inputs[IN_12].getChannels() });
    for(int triggerPortId=0; triggerPortId<12; triggerPortId++){
      Input port = inputs[IN_1+triggerPortId];
      for(int i=0; i<channels; i++) {
        float data = 0.f;
        if (port.isPolyphonic()){
          data = port.getNormalPolyVoltage(0.f, i);
        } else if (i==0) {
          data = port.getNormalVoltage(0.f);
        }
        if (trigger[triggerPortId][i].process(data)) {
          switch(currentRangeMode) {
          case ONE_VOLT_PER_OCTAVE:
            outVolts[i] = triggerPortId / 12.0;
            break;
          default:
            // TEN_VOLTS
            outVolts[i] = (triggerPortId / 10.0) * 10.0;
          }
        }
      }
    }
    // RANGE mode toggle button
    if (tapRangeTrigger.process(params[TAP_RANGE].getValue())) {
      rangeLightPulse.trigger(LIGHT_DURATION);
      setRangeMode(currentRangeMode + 1);
      reset();
    }
    lights[TAP_RANGE_LIGHT].setBrightness(rangeLightPulse.process(args.sampleTime) ? 1.f : 0.f);
    // RESET
    if (resetTrigger.process(inputs[RESET].getNormalVoltage(0.0))) {
      reset();
    }

    outputs[OUT].setChannels(channels);
    for (int i=0; i<channels; i++){
      outputs[OUT].setVoltage(outVolts[i], i);
    }
  }

  void setRangeMode(int rangeMode) {
    currentRangeMode = rangeMode % NUM_RANGE_MODES;
    std::string musicNoteLabels[12] = {"C", "C#", "D", "D#", "E", "F",
                                       "F#", "G", "G#", "A", "A#", "B"};
    for (int i=0; i<12; i++) {
      triggerLabels[i] = std::string("");
    }
    switch(currentRangeMode) {
    case ONE_VOLT_PER_OCTAVE:
      for(int i=0; i<12; i++){
        triggerLabels[i] = musicNoteLabels[i];
      }
      triggerLabelFontSize = 12;
      break;
    default:
      // Default is 10v range (TEN_VOLTS)
      for(int i=0; i<=10; i++){
        triggerLabels[i] = string::f("%dV", i);
      }
      triggerLabelFontSize = 12;
    }
  }

  void reset(bool init) {
    setRangeMode(currentRangeMode);
    for(int i=0;i<16;i++) {
      outVolts[i] = 0;
    }
    if (init) {
      setRangeMode(TEN_VOLTS);
    }
  }
  void reset() { reset(0); }
  void onReset() override { reset(1); }

  json_t *dataToJson() override {
    json_t *rootJ = json_object();
    json_object_set_new(rootJ, "currentRangeMode", json_integer(currentRangeMode));
    return rootJ;
  }

  void dataFromJson(json_t *rootJ) override {
    currentRangeMode = json_integer_value(json_object_get(rootJ, "currentRangeMode"));
    setRangeMode(currentRangeMode);
  }

};


#define HP 6
#define ROWS 14
#define COLUMNS 11
panel_grid<HP, ROWS, COLUMNS> rangeGrid;
Vec top = Vec(-5, 0);
Vec tapRangeLoc = top.plus(rangeGrid.loc(2, 3));
Vec resetLoc = top.plus(rangeGrid.loc(4, 3));
Vec outLoc = top.plus(rangeGrid.loc(6, 3));
Vec in1Loc = top.plus(rangeGrid.loc(1, 9));
Vec in2Loc = top.plus(rangeGrid.loc(2, 9));
Vec in3Loc = top.plus(rangeGrid.loc(3, 9));
Vec in4Loc = top.plus(rangeGrid.loc(4, 9));
Vec in5Loc = top.plus(rangeGrid.loc(5, 9));
Vec in6Loc = top.plus(rangeGrid.loc(6, 9));
Vec in7Loc = top.plus(rangeGrid.loc(7, 9));
Vec in8Loc = top.plus(rangeGrid.loc(8, 9));
Vec in9Loc = top.plus(rangeGrid.loc(9, 9));
Vec in10Loc = top.plus(rangeGrid.loc(10, 9));
Vec in11Loc = top.plus(rangeGrid.loc(11, 9));
Vec in12Loc = top.plus(rangeGrid.loc(12, 9));


struct RangeTriggerOverlay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  Range *module;
  int fontSize;

  std::string *triggerLabels;
  std::string exampleTriggerLabels[12];

  RangeTriggerOverlay(int hp_width) : DynamicOverlay(hp_width) {
    for(int i=0; i<=10; i++){
      exampleTriggerLabels[i] = string::f("%dV", i);
    }
  }
  void drawTriggerLabels() {
    Vec numOffset = Vec(-20, 3);
    addText(triggerLabels[0], fontSize, in1Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[1], fontSize, in2Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[2], fontSize, in3Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[3], fontSize, in4Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[4], fontSize, in5Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[5], fontSize, in6Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[6], fontSize, in7Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[7], fontSize, in8Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[8], fontSize, in9Loc.plus(numOffset), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[9], fontSize, in10Loc.plus(numOffset).plus((triggerLabels[9].size() > 2 && fontSize > 9) ? Vec(-2,0) : Vec(0,0)), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[10], fontSize, in11Loc.plus(numOffset).plus((triggerLabels[10].size() > 2 && fontSize > 9) ? Vec(-3,0) : Vec(0,0)), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
    addText(triggerLabels[11], fontSize, in12Loc.plus(numOffset).plus((triggerLabels[11].size() > 2 && fontSize > 9) ? Vec(-3,0) : Vec(0,0)), BLACK,
            RED_TRANSPARENT, FANTASQUE, 0);
  }

  void draw(const DrawArgs &args) override {
    DynamicOverlay::clear();
    if (module) {
      triggerLabels = module->triggerLabels;
      fontSize = module->triggerLabelFontSize;
    } else {
      // Draw example display for module browser:
      triggerLabels = exampleTriggerLabels;
      fontSize = 12;
    }
    drawTriggerLabels();
    DynamicOverlay::draw(args);
  }
};


struct RangeWidget : ModuleWidget {

  RangeWidget(Range *module) {
    setModule(module);
    setPanel(
             APP->window->loadSvg(asset::plugin(pluginInstance, "res/6hp.svg")));

    addInput(createInputCentered<PJ301MPort>(resetLoc, module, Range::RESET));
    addInput(createInputCentered<PJ301MPort>(in1Loc, module, Range::IN_1));
    addInput(createInputCentered<PJ301MPort>(in2Loc, module, Range::IN_2));
    addInput(createInputCentered<PJ301MPort>(in3Loc, module, Range::IN_3));
    addInput(createInputCentered<PJ301MPort>(in4Loc, module, Range::IN_4));
    addInput(createInputCentered<PJ301MPort>(in5Loc, module, Range::IN_5));
    addInput(createInputCentered<PJ301MPort>(in6Loc, module, Range::IN_6));
    addInput(createInputCentered<PJ301MPort>(in7Loc, module, Range::IN_7));
    addInput(createInputCentered<PJ301MPort>(in8Loc, module, Range::IN_8));
    addInput(createInputCentered<PJ301MPort>(in9Loc, module, Range::IN_9));
    addInput(createInputCentered<PJ301MPort>(in10Loc, module, Range::IN_10));
    addInput(createInputCentered<PJ301MPort>(in11Loc, module, Range::IN_11));
    addInput(createInputCentered<PJ301MPort>(in12Loc, module, Range::IN_12));
    addOutput(createOutputCentered<PJ301MPort>(outLoc, module, Range::OUT));
    addParam(
        createParamCentered<LEDBezel>(tapRangeLoc, module, Range::TAP_RANGE));
    addChild(createLightCentered<LEDBezelLight<BlueLight>>(
        tapRangeLoc, module, Range::TAP_RANGE_LIGHT));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Range", 30, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("RANGE", 13, tapRangeLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 1);
      overlay->addText("RESET", 13, resetLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 1);
      overlay->addText("OUT", 13, outLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT, FANTASQUE, 1);
      buffer->addChild(overlay);
      addChild(buffer);
    }
    // Draw dynamic trigger labels
    {
      RangeTriggerOverlay *triggerOverlay = new RangeTriggerOverlay(HP);
      triggerOverlay->module = module;
      addChild(triggerOverlay);
    }

  }

  void appendContextMenu(Menu *menu) override {
    Range *module = dynamic_cast<Range *>(this->module);
    assert(module);

    menu->addChild(new MenuSeparator());
    MenuLabel *settingsLabel = new MenuLabel();
    settingsLabel->text = "Settings";
    menu->addChild(settingsLabel);
  }
};

Model *modelRange = createModel<Range, RangeWidget>("Range");

