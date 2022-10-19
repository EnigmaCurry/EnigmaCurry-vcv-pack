/*
 * Trigger a gate signal with a clocked length.
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
#define TRIGGER_DURATION 1e-3f

struct Pulse : Module {
  enum ParamIds { LEN, NUM_PARAMS };
  enum InputIds { TRIG, RESET, CLOCK, NUM_INPUTS };
  enum OutputIds { GATE, END, NUM_OUTPUTS };
  enum LightIds { NUM_LIGHTS };
  enum OnRetriggerActionIds { ON_RETRIGGER_NEW_TRIGGER, ON_RETRIGGER_NO_NEW_TRIGGER, ON_RETRIGGER_RESET};
  dsp::SchmittTrigger clockTrigger, trigTrigger[16], resetTrigger;
  dsp::PulseGenerator endPulse[16];
  int pulseLength = 1;
  int pulseCountMod = 0;
  int clockDivider = 1;
  int clockCountMod = 0;
  int trigQuantize = 1;
  int onRetriggerAction = 0;
  int trigQueue[16] = {-1};
  int gateProgress[16] = {-1};
  bool unclockedTrigs[16] = {false};

  Pulse() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(Pulse::LEN, 1, 128, 4, "Pulse Length");
    configInput(CLOCK, "CLOCK");
    configInput(TRIG, "TRIG");
    configInput(RESET, "RESET");
    configOutput(GATE, "GATE");
    configOutput(END, "END");
    reset();
  }

  void process(const ProcessArgs &args) override {
    int channels = inputs[TRIG].getChannels();
    outputs[GATE].setChannels(channels);
    outputs[END].setChannels(channels);
    // LENGTH
    pulseLength = int(params[LEN].getValue());
    // CLOCK
    if (clockTrigger.process(inputs[CLOCK].getNormalVoltage(0.0))) {
      clockCountMod = (clockCountMod + 1) % clockDivider;
      if (clockCountMod == 0) {
        pulseCountMod = (pulseCountMod + 1) % (trigQuantize < 1 ? 1 : trigQuantize);
        // Clocked Gate Progress:
        for(int i=0; i < 16; i++) {
          // Reset the unclocked trigs that occured before the clock:
          unclockedTrigs[i] = false;
          if (i < channels && trigTrigger[i].process(inputs[TRIG].getNormalPolyVoltage(0.0, i) >= 1.f)) {
            // Handle trigs coincidential with the clock rising edge:
            trigQueue[i] = 0;
          }
          if (gateProgress[i] == pulseLength - 1) {
            // Pulse finished:
            gateProgress[i] = -1;
          } else if (gateProgress[i] >= 0) {
            // Pulse ongoing:
            gateProgress[i] += 1;
          }
          if (trigQueue[i] < 0) {
            // No trig scheduled
          } else if (trigQueue[i] > 0) {
            // Trig is scheduled, but its not time yet:
            trigQueue[i] -= 1;
          } else {
            //Time to trig now - trigQueue[i] == 0
            if (gateProgress[i] == -1 || \
                onRetriggerAction == ON_RETRIGGER_NEW_TRIGGER) {
              gateProgress[i] = 0;
            } else if (onRetriggerAction == ON_RETRIGGER_RESET) {
              gateProgress[i] = -1;
            }
            trigQueue[i] = -1;
          }
        }
      }
      // END
      if (clockCountMod == clockDivider - 1) {
        for (int i=0; i<channels; i++) {
          if (gateProgress[i] == pulseLength - 1) {
            endPulse[i].trigger(TRIGGER_DURATION);
          }
        }
      }
    } else {
      // Handle trigs that occur in between clock pulses:
      for (int i = 0; i < channels; i++) {
        if (i < channels && trigTrigger[i].process(inputs[TRIG].getNormalPolyVoltage(0.0, i) >= 1.f)) {
          trigQueue[i] = (trigQuantize < 1 ? 1 : trigQuantize) - (pulseCountMod == -1 ? 0 : pulseCountMod) - 1;
          unclockedTrigs[i] = trigQuantize == 0;
        }
      }
    }
    // RESET
    if (resetTrigger.process(inputs[RESET].getNormalVoltage(0.0))) {
      reset();
    }
    for (int i=0; i < 16; i++) {
      // GATE:
      outputs[GATE].setVoltage((unclockedTrigs[i] || gateProgress[i] >= 0) ? 10.f : 0.f, i);
      // END trigger:
      outputs[END].setVoltage(endPulse[i].process(args.sampleTime) ? 10.f : 0.f, i);
    }
  }

  void reset(bool init) {
    clockCountMod = -1;
    pulseCountMod = -1;
    for (int i = 0; i < 16; i++) {
      trigQueue[i] = -1;
      gateProgress[i] = -1;
      unclockedTrigs[i] = false;
    }
    if (init) {
      clockDivider = 1;
      trigQuantize = 1;
      onRetriggerAction = 0;
    }
  }
  void reset() { reset(0); }
  void onReset() override { reset(1); }

  json_t *dataToJson() override {
    json_t *rootJ = json_object();
    json_object_set_new(rootJ, "clockDivider", json_integer(clockDivider));
    json_object_set_new(rootJ, "trigQuantize", json_integer(trigQuantize));
    json_object_set_new(rootJ, "onRetriggerAction", json_integer(onRetriggerAction));
    return rootJ;
  }

  void dataFromJson(json_t *rootJ) override {
    clockDivider = json_integer_value(json_object_get(rootJ, "clockDivider"));
    trigQuantize = json_integer_value(json_object_get(rootJ, "trigQuantize"));
    onRetriggerAction = json_integer_value(json_object_get(rootJ, "onRetriggerAction"));
  }

};


#define HP 3
#define ROWS 256
#define COLUMNS 1
panel_grid<HP, ROWS, COLUMNS> pulseGrid;

std::string pulsePad(int num, int wide) {
  if (num >= pow(10,wide)) {
    if (wide < 2) {
      return "X";
    } else if (wide == 2) {
      return "XX";
    } else {
      return "XXX";
    }
  }
  else if (num <= 0) {
    if (wide < 2) {
      return "_";
    } else if (wide == 2) {
      return "__";
    } else {
      return "___";
    }
  }
  std::string s = std::to_string(num);
  if (int(s.length()) < wide) {
    s.insert(s.begin(), wide - s.length(), '_');
  }
  return s;
}
std::string pulsePad(int num) {
  return pulsePad(num, 3);
}


struct PulseDisplay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  Pulse *module;

  std::string pad(int num) {
    return pulsePad(num);
  }

  PulseDisplay(int hp_width) : DynamicOverlay(hp_width) {}

  void draw(const DrawArgs &args) override {
    Vec displayLoc = pulseGrid.loc(20,0).plus(Vec(0,6));
    Vec lengthTextLoc = displayLoc.plus(Vec(0,8));
    Vec pulseCountTextLoc = displayLoc.plus(Vec(0,24));
    Vec clockCountTextLoc = displayLoc.plus(Vec(0, 40));
    if (module) {
      DynamicOverlay::clear();
      addText(pulsePad(module->pulseLength), 12, lengthTextLoc,
              RED, CLEAR, DSEG);
      int channelsActive = 0;
      std::string pulseCountText = "";
      for (int i=0; i < 16; i++) {
        if(module->gateProgress[i] >= 0) {
          channelsActive += 1;
          pulseCountText = pulseCountText == "" ? pulsePad(module->gateProgress[i] + 1) : pulseCountText;
        }
      }
      addText(pulseCountText, 12, pulseCountTextLoc,
              channelsActive > 1 ? PINK : YELLOW, CLEAR, DSEG);
      if (module->clockCountMod >= 0 && module->clockDivider > 1) {
        int wide = module->clockDivider > 99 ? 3 : (module->clockDivider > 9 ? 2 : 1);
        addText(string::f("%s/%d", pulsePad(module->clockCountMod + 1, wide).c_str(), module->clockDivider),
                wide == 3 ? 6 : (wide == 2 ? 8 : 12), clockCountTextLoc, WHITE, CLEAR, DSEG);
      }
      DynamicOverlay::draw(args);
    } else {
      // Draw example display for module browser:
      DynamicOverlay::clear();
      addText(pulsePad(4), 12, lengthTextLoc,
              RED, CLEAR, DSEG);
      addText(pulsePad(3), 12, pulseCountTextLoc,
              YELLOW, CLEAR, DSEG);
      addText(string::f("%s/%d", pulsePad(12, 2).c_str(), 24),
                8, clockCountTextLoc, WHITE, CLEAR, DSEG);
      DynamicOverlay::draw(args);
    }
  }
};

struct PulseWidget : ModuleWidget {
  Vec displayLoc = pulseGrid.loc(20,0).plus(Vec(0,6));
  Vec lenLoc = pulseGrid.loc(76,0);
  int o = 105;
  int h = 28;
  Vec clockLoc = pulseGrid.loc(o, 0);
  Vec trigLoc = pulseGrid.loc(o+h, 0);

  Vec resetLoc = pulseGrid.loc(o+h*2, 0);
  Vec gateLoc = pulseGrid.loc(o+h*3,0);
  Vec endLoc = pulseGrid.loc(o+h*4,0);
  // Vec pulseDisplayTopLeft = pulseGrid.loc(, 0).minus(Vec(7,10));
  // Vec pulseDisplayBottomRight = pulseGrid.loc(3, 1).plus(Vec(3,10));

  PulseWidget(Pulse *module) {
    setModule(module);
    addInput(createInputCentered<PJ301MPort>(trigLoc, module, Pulse::TRIG));
    addInput(createInputCentered<PJ301MPort>(resetLoc, module, Pulse::RESET));
    addInput(createInputCentered<PJ301MPort>(clockLoc, module, Pulse::CLOCK));
    addOutput(createOutputCentered<PJ301MPort>(gateLoc, module, Pulse::GATE));
    addOutput(createOutputCentered<PJ301MPort>(endLoc, module, Pulse::END));
    setPanel(
             APP->window->loadSvg(asset::plugin(pluginInstance, "res/3hp.svg")));
    addParam(
             createParamCentered<RoundBlackKnob>(lenLoc, module, Pulse::LEN));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Pulse", 20, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("LENGTH", 9, lenLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 0);
      overlay->addText("CLOCK", 9, clockLoc.minus(Vec(0, 15)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 0);
      overlay->addText("TRIG", 9, trigLoc.minus(Vec(0, 15)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 0);
      overlay->addText("RESET", 9, resetLoc.minus(Vec(0, 15)), WHITE,
                       RED_TRANSPARENT, FANTASQUE, 0);
      overlay->addText("GATE", 9, gateLoc.minus(Vec(0, 15)), WHITE,
                       BLACK_TRANSPARENT, FANTASQUE, 0);
      overlay->addText("END", 9, endLoc.minus(Vec(0, 15)), WHITE,
                       BLACK_TRANSPARENT, FANTASQUE, 0);
      buffer->addChild(overlay);
      addChild(buffer);
      overlay->addBox(displayLoc.minus(Vec(18,8)),
                      Vec(36,52),
                      BLACK_TRANSPARENT, 5);
    }
    // Draw dynamic counter display
    {
      PulseDisplay *overlay = new PulseDisplay(HP);
      overlay->module = module;
      addChild(overlay);
    }
  }

  void appendContextMenu(Menu *menu) override {
    Pulse *module = dynamic_cast<Pulse *>(this->module);
    assert(module);

    menu->addChild(new MenuSeparator());
    MenuLabel *settingsLabel = new MenuLabel();
    settingsLabel->text = "Settings";
    menu->addChild(settingsLabel);


    struct ClockDividerValueItem : MenuItem {
      Pulse *module;
      int value;
      void onAction(const event::Action &e) override {
        module->clockDivider = value;
      }
    };
    struct ClockDividerItem : MenuItem {
      Pulse *module;
      void createClockDividerSelection(Menu *menu, Pulse *module,
                                      std::string label, int value) {
        ClockDividerValueItem *item = createMenuItem<ClockDividerValueItem>(
            label, CHECKMARK(module->clockDivider == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createClockDividerSelection(menu, module, "1", 1);
        createClockDividerSelection(menu, module, "2", 2);
        createClockDividerSelection(menu, module, "4 (MIDI-CV CLK/N)", 4);
        createClockDividerSelection(menu, module, "8", 8);
        createClockDividerSelection(menu, module, "12", 12);
        createClockDividerSelection(menu, module, "16", 16);
        createClockDividerSelection(menu, module, "32", 32);
        createClockDividerSelection(menu, module, "64", 64);
        createClockDividerSelection(menu, module, "96 (24 PPQN, MIDI-CV CLK)",
                                    24 * 4);
        createClockDividerSelection(menu, module, "128", 128);
        createClockDividerSelection(menu, module, "192 (48 PPQN)", 48 * 4);
        return menu;
      }
    };
    ClockDividerItem *clockDividerItem =
        createMenuItem<ClockDividerItem>("Clock Divider", RIGHT_ARROW);
    clockDividerItem->module = module;
    menu->addChild(clockDividerItem);

    struct QuantizeTrigValueItem : MenuItem {
      Pulse *module;
      int value;
      void onAction(const event::Action &e) override {
        module->trigQuantize = value;
      }
    };
    struct QuantizeTrigItem : MenuItem {
      Pulse *module;
      void createQuantizeTrigSelection(Menu *menu, Pulse *module,
                                      std::string label, int value) {
        QuantizeTrigValueItem *item = createMenuItem<QuantizeTrigValueItem>(
            label, CHECKMARK(module->trigQuantize == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createQuantizeTrigSelection(menu, module, "OFF", 0);
        createQuantizeTrigSelection(menu, module, "1", 1);
        createQuantizeTrigSelection(menu, module, "2", 2);
        createQuantizeTrigSelection(menu, module, "4", 4);
        createQuantizeTrigSelection(menu, module, "8", 8);
        createQuantizeTrigSelection(menu, module, "16", 16);
        createQuantizeTrigSelection(menu, module, "32", 32);
        createQuantizeTrigSelection(menu, module, "64", 64);
        createQuantizeTrigSelection(menu, module, "128", 128);
        return menu;
      }
    };
    QuantizeTrigItem *quantizeTrigItem =
        createMenuItem<QuantizeTrigItem>("Trig Quantize", RIGHT_ARROW);
    quantizeTrigItem->module = module;
    menu->addChild(quantizeTrigItem);

  struct OnRetriggerValueItem : MenuItem {
      Pulse *module;
      int value;
      void onAction(const event::Action &e) override {
        module->onRetriggerAction = value;
      }
    };
    struct OnRetriggerItem : MenuItem {
      Pulse *module;
      void createOnRetriggerSelection(Menu *menu, Pulse *module,
                                      std::string label, int value) {
        OnRetriggerValueItem *item = createMenuItem<OnRetriggerValueItem>(
            label, CHECKMARK(module->onRetriggerAction == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createOnRetriggerSelection(menu, module, "New Trigger", Pulse::ON_RETRIGGER_NEW_TRIGGER);
        createOnRetriggerSelection(menu, module, "No New Trigger", Pulse::ON_RETRIGGER_NO_NEW_TRIGGER);
        createOnRetriggerSelection(menu, module, "Reset", Pulse::ON_RETRIGGER_RESET);
        return menu;
      }
    };
    OnRetriggerItem *onRetriggerItem =
        createMenuItem<OnRetriggerItem>("On Re-trigger during gate", RIGHT_ARROW);
    onRetriggerItem->module = module;
    menu->addChild(onRetriggerItem);
  }
};

Model *modelPulse = createModel<Pulse, PulseWidget>("Pulse");

