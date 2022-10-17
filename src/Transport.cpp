/*
 * DAW-like transport play/stop/record for X bars
 * Copyright (C) 2021-2022 EnigmaCurry
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

struct Transport : Module {
  enum ParamIds { LEN, TAP_LEN, TAP_PLAY, TAP_ARM, TAP_RESET, NUM_PARAMS };
  enum InputIds { PLAY, ARM, CLK, RESET, NUM_INPUTS };
  enum OutputIds { PGAT, PTRG, RST, RGAT, RTRG, LOOP, NUM_OUTPUTS };
  enum LightIds { TAP_LEN_LIGHT, TAP_PLAY_LIGHT, TAP_ARM_LIGHT, TAP_RESET_LIGHT, NUM_LIGHTS };
  enum OnStartActionIds { ON_START_NO_ACTION, ON_START_RESET };
  enum OnStopActionIds { ON_STOP_NO_ACTION, ON_STOP_RESET };
  int recordLength = 0;
  bool bypassRecordLength = true;
  int playCount = 0;
  int recCount = 0;
  bool playing = false;
  bool armed = false;
  int armQuantize = 1;
  bool toFlipArm = false;
  bool recordLengthIsPlayLength = false;
  float blinkPhase = 0.f;
  float resetLightTime = 0.f;
  int clockDivider = 4;
  int clockCount = 0;
  bool playIsIdempotent = false;
  int onStartActions = ON_START_NO_ACTION;
  int onStopActions = ON_STOP_RESET;
  dsp::SchmittTrigger clockTrigger, playTrigger, armTrigger, resetTrigger;
  dsp::BooleanTrigger tapLenTrigger, tapPlayTrigger, tapArmTrigger, tapResetTrigger;
  dsp::PulseGenerator playPulse, recordPulse, resetPulse, loopPulse;

  Transport() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(Transport::LEN, 0, 128, 4, "Record Length");
    configParam(TAP_LEN, 0.f, 1.f, 0.f, "Punch out recording after length");
    configParam(TAP_PLAY, 0.f, 1.f, 0.f, "Tap to play");
    configParam(TAP_ARM, 0.f, 1.f, 0.f, "Tap to arm record");
    configParam(TAP_RESET, 0.f, 1.f, 0.f, "Tap to reset");
    configInput(CLK, "Clock");
    configInput(PLAY, "Play");
    configInput(RESET, "Reset / Stop");
    configInput(ARM, "Arm Recording");
    configOutput(LOOP, "Loop Trigger");
    configOutput(PGAT, "Play Gate");
    configOutput(PTRG, "Play Trigger");
    configOutput(RGAT, "Record Gate");
    configOutput(RTRG, "Record Trigger");
    configOutput(RST, "Reset Trigger");
    reset(1);
  }

  void process(const ProcessArgs &args) override {
    if (resetLightTime > 0.f) {
      lights[TAP_RESET_LIGHT].setBrightness(1.f);
      resetLightTime -= args.sampleTime;
    } else {
      lights[TAP_RESET_LIGHT].setBrightness(0.f);
    }

    recordLength = int(params[LEN].getValue());
    if (tapLenTrigger.process(params[TAP_LEN].getValue())) {
      bypassRecordLength = !bypassRecordLength;
    }
    //RESET
    if (resetTrigger.process(inputs[RESET].getNormalVoltage(0.0)) ||
        tapResetTrigger.process(params[TAP_RESET].getValue())) {
      triggerResetPulse();
      reset();
      return;
    }
    // PLAY
    if (playTrigger.process(inputs[PLAY].getNormalVoltage(0.0)) ||
        tapPlayTrigger.process(params[TAP_PLAY].getValue())) {
      if (!playIsIdempotent || !playing) {
        playing = !playing;
        if (playing && onStartActions == ON_START_RESET) {
          triggerResetPulse();
        } else if (!playing && onStopActions == ON_STOP_RESET) {
          triggerResetPulse();
          reset();
        }
        playPulse.trigger(TRIGGER_DURATION);
        if (armed)
          recordPulse.trigger(TRIGGER_DURATION);
      }
    }
    // (unclocked) ARM
    if (armTrigger.process(inputs[ARM].getNormalVoltage(0.0)) ||
        tapArmTrigger.process(params[TAP_ARM].getValue() > 0.f)) {
      if (armQuantize == 0) {
        armed = !armed;
        toFlipArm = false;
        if (playing)
          recordPulse.trigger(TRIGGER_DURATION);
      } else {
        if (playing) {
          toFlipArm = !toFlipArm;
        } else {
          armed = !armed;
          toFlipArm = false;
        }
      }
    }
    // CLOCK and (clocked) ARM
    if (playing && clockTrigger.process(inputs[CLK].getNormalVoltage(0.0))) {
      clockCount += 1;
      if (clockCount % clockDivider == 0) {
        clockCount = 0;
        if (!recordLengthIsPlayLength || bypassRecordLength ||
            recordLength == 0 || recCount < recordLength) {
          playCount = (playCount + 1) % 1000;
          if (playCount % recordLength == 1) {
            triggerLoopPulse();
          }
          toFlipArm =
            !bypassRecordLength && (recCount == recordLength + 1 - armQuantize)
            ? true
            : toFlipArm;
          recCount += armed ? 1 : 0;
        } else {
          triggerResetPulse();
          reset();
        }
        if (toFlipArm) {
          if (armQuantize <= 0 || (playCount - 1) % armQuantize == 0) {
            armed = !armed;
            toFlipArm = false;
            recCount = armed ? 1 : 0;
            recordPulse.trigger(TRIGGER_DURATION);
          }
        }
      } else {
        // Skipping this clock cycle
      }
    } else if (!playing && !armed) {
      reset();
    }

    blinkPhase += args.sampleTime * 2;
    if (blinkPhase >= 1.f)
      blinkPhase -= 1.f;

    outputs[PGAT].setVoltage(10 * playing);
    outputs[RGAT].setVoltage(10 * (playing && armed));
    lights[TAP_LEN_LIGHT].setBrightness(!bypassRecordLength);
    lights[TAP_PLAY_LIGHT].setBrightness(playing);
    if ((playing && toFlipArm) || (!playing && armed)) {
      lights[TAP_ARM_LIGHT].setBrightness(blinkPhase < 0.5f ? 1.f : 0.f);
    } else {
      lights[TAP_ARM_LIGHT].setBrightness(armed);
    }

    outputs[PTRG].setVoltage(playPulse.process(args.sampleTime) ? 10.f : 0.f);
    outputs[RTRG].setVoltage(recordPulse.process(args.sampleTime) ? 10.f : 0.f);
    outputs[RST].setVoltage(resetPulse.process(args.sampleTime) ? 10.f : 0.f);
    outputs[LOOP].setVoltage(loopPulse.process(args.sampleTime) ? 10.f : 0.f);
  }

  void triggerResetPulse() {
    resetLightTime = 0.1f;
    resetPulse.trigger(TRIGGER_DURATION);
  }

  void triggerLoopPulse() {
    loopPulse.trigger(TRIGGER_DURATION);
  }

  void reset(bool init) {
    playCount = 0;
    recCount = 0;
    clockCount = -1;
    if (playing && !init) {
      playPulse.trigger(TRIGGER_DURATION);
      triggerResetPulse();
    }
    playing = false;
    if (armed)
      recordPulse.trigger(TRIGGER_DURATION);
    armed = false;
    toFlipArm = false;
    if (init) {
      armQuantize = 1;
      clockDivider = 4;
      bypassRecordLength = true;
      recordLengthIsPlayLength = false;
      playIsIdempotent = false;
      onStartActions = ON_START_NO_ACTION;
      onStopActions = ON_STOP_RESET;
    }
  }
  void reset() { reset(0); }
  void onReset() override { reset(1); }

  json_t *dataToJson() override {
    json_t *rootJ = json_object();
    json_object_set_new(rootJ, "bypassRecordLength",
                        json_integer((int)bypassRecordLength));
    json_object_set_new(rootJ, "armed", json_integer((int)armed));
    json_object_set_new(rootJ, "playing", json_integer((int)playing));
    json_object_set_new(rootJ, "armQuantize", json_integer(armQuantize));
    json_object_set_new(rootJ, "clockDivider", json_integer(clockDivider));
    json_object_set_new(rootJ, "playIsIdempotent", json_integer(playIsIdempotent));
    json_object_set_new(rootJ, "onStartActions", json_integer(onStartActions));
    json_object_set_new(rootJ, "onStopActions", json_integer(onStopActions));
    json_object_set_new(rootJ, "recordLengthIsPlayLength",
                        json_integer(recordLengthIsPlayLength));
    return rootJ;
  }

  void dataFromJson(json_t *rootJ) override {
    json_t *bypassJ = json_object_get(rootJ, "bypassRecordLength");
    bypassRecordLength = (bool)json_integer_value(bypassJ);

    json_t *armedJ = json_object_get(rootJ, "armed");
    armed = (bool)json_integer_value(armedJ);
    json_t *playingJ = json_object_get(rootJ, "playing");
    playing = (bool)json_integer_value(playingJ);
    json_t *armQuantizeJ = json_object_get(rootJ, "armQuantize");
    armQuantize = json_integer_value(armQuantizeJ);
    json_t *clockDividerJ = json_object_get(rootJ, "clockDivider");
    clockDivider = json_integer_value(clockDividerJ);
    json_t *playIsIdempotentJ = json_object_get(rootJ, "playIsIdempotent");
    playIsIdempotent = json_integer_value(playIsIdempotentJ);
    json_t *onStartActionsJ = json_object_get(rootJ, "onStartActions");
    onStartActions = json_integer_value(onStartActionsJ);
    json_t *onStopActionsJ = json_object_get(rootJ, "onStopActions");
    onStopActions = json_integer_value(onStopActionsJ);
    json_t *recordLengthIsPlayLengthJ =
        json_object_get(rootJ, "recordLengthIsPlayLength");
    recordLengthIsPlayLength =
        (bool)json_integer_value(recordLengthIsPlayLengthJ);
  }
};

#define HP 10
#define ROWS 24
#define COLUMNS 10
panel_grid<HP, ROWS, COLUMNS> transportGrid;

struct TransportDisplay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  Transport *module;

  TransportDisplay(int hp_width) : DynamicOverlay(hp_width) {}

  std::string pad(int num) {
    if (num > 999)
      return "XXX";
    else if (num == 0)
      return "___";
    std::string s = std::to_string(num);
    if (s.length() < 3) {
      s.insert(s.begin(), 3 - s.length(), '_');
    }
    return s;
  }

  void draw(const DrawArgs &args) override {
    Vec topTextLoc = transportGrid.loc(4, 7).minus(Vec(8, -15));
    Vec bottomTextLoc = transportGrid.loc(8, 7).minus(Vec(8, -15));
    if (module) {
      DynamicOverlay::clear();
      std::string recordLength = pad(module->recordLength);
      std::string playCount = pad(module->playCount);
      std::string recCount = pad(module->recCount);

      addText(recordLength, 25, topTextLoc,
              (!module->bypassRecordLength && module->recordLength > 0) ? RED
              : WHITE,
              CLEAR, DSEG);
      if (module->armed) {
        addText(recCount, 25, bottomTextLoc, RED, CLEAR,
                DSEG);
      } else {
        addText(playCount, 25, bottomTextLoc, WHITE, CLEAR,
                DSEG);
      }
      DynamicOverlay::draw(args);
    } else {
      // Draw example when module is inacative (for module browser):
      addText(pad(4), 25, topTextLoc,
              WHITE, CLEAR, DSEG);
      addText(pad(13), 25, bottomTextLoc, WHITE, CLEAR,
              DSEG);
      DynamicOverlay::draw(args);
    }
  }
};

struct TransportWidget : ModuleWidget {
  TransportWidget(Transport *module) {
    setModule(module);
    setPanel(
        APP->window->loadSvg(asset::plugin(pluginInstance, "res/10hp.svg")));

    Vec lenLoc = transportGrid.loc(4, 1);
    Vec tapLenLoc = transportGrid.loc(4, 3);
    Vec clkLoc = transportGrid.loc(8, 1);
    Vec loopLoc = transportGrid.loc(8, 3);
    Vec rstLoc = transportGrid.loc(20, 6);
    Vec playLoc = transportGrid.loc(12, 1);
    Vec tapPlayLoc = transportGrid.loc(12, 3);
    Vec pgatLoc = transportGrid.loc(12, 6);
    Vec ptrgLoc = transportGrid.loc(12, 8);
    Vec armRecLoc = transportGrid.loc(16, 1);
    Vec tapArmLoc = transportGrid.loc(16, 3);
    Vec rgatLoc = transportGrid.loc(16, 6);
    Vec rtrgLoc = transportGrid.loc(16, 8);
    Vec resetLoc = transportGrid.loc(20, 1);
    Vec tapResetLoc = transportGrid.loc(20, 3);
    Vec lengthDisplayLoc = transportGrid.loc(4, 7);
    Vec playLengthDisplayLoc = transportGrid.loc(8, 7);

    addParam(
        createParamCentered<RoundBlackKnob>(lenLoc, module, Transport::LEN));
    addParam(
        createParamCentered<LEDBezel>(tapLenLoc, module, Transport::TAP_LEN));
    addChild(createLightCentered<LEDBezelLight<GreenLight>>(
        tapLenLoc, module, Transport::TAP_LEN_LIGHT));
    addParam(
        createParamCentered<LEDBezel>(tapPlayLoc, module, Transport::TAP_PLAY));
    addChild(createLightCentered<LEDBezelLight<GreenLight>>(
        tapPlayLoc, module, Transport::TAP_PLAY_LIGHT));
    addParam(
        createParamCentered<LEDBezel>(tapArmLoc, module, Transport::TAP_ARM));
    addChild(createLightCentered<LEDBezelLight<RedLight>>(
        tapArmLoc, module, Transport::TAP_ARM_LIGHT));
    addParam(
        createParamCentered<LEDBezel>(tapResetLoc, module, Transport::TAP_RESET));
    addChild(createLightCentered<LEDBezelLight<GreenLight>>(
        tapResetLoc, module, Transport::TAP_RESET_LIGHT));

    addInput(createInputCentered<PJ301MPort>(clkLoc, module, Transport::CLK));
    addOutput(createOutputCentered<PJ301MPort>(loopLoc, module, Transport::LOOP));
    addInput(createInputCentered<PJ301MPort>(playLoc, module, Transport::PLAY));
    addInput(
        createInputCentered<PJ301MPort>(armRecLoc, module, Transport::ARM));
    addOutput(
        createOutputCentered<PJ301MPort>(ptrgLoc, module, Transport::PTRG));
    addOutput(createOutputCentered<PJ301MPort>(rstLoc, module, Transport::RST));
    addOutput(
        createOutputCentered<PJ301MPort>(pgatLoc, module, Transport::PGAT));
    addOutput(
        createOutputCentered<PJ301MPort>(rgatLoc, module, Transport::RGAT));
    addOutput(
        createOutputCentered<PJ301MPort>(rtrgLoc, module, Transport::RTRG));
    addInput(
        createInputCentered<PJ301MPort>(resetLoc, module, Transport::RESET));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Transport", 40, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("LENGTH", 16, lenLoc.minus(Vec(-15, 22)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("CLK", 12, clkLoc.minus(Vec(0, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("LOOP", 12, loopLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("RST", 12, rstLoc.minus(Vec(0, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("PLAY", 20, playLoc.minus(Vec(-15, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("ARM", 20, armRecLoc.minus(Vec(-15, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("PGAT", 12, pgatLoc.minus(Vec(1, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("PTRG", 12, ptrgLoc.minus(Vec(-1, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("RGAT", 12, rgatLoc.minus(Vec(1, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("RTRG", 12, rtrgLoc.minus(Vec(-1, 20)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("RESET", 20, resetLoc.minus(Vec(-15, 20)), WHITE,
                       RED_TRANSPARENT);
      overlay->addBox(lengthDisplayLoc.plus(Vec(-43, -35)),
                      playLengthDisplayLoc.plus(Vec(-40, -15)),
                      BLACK_TRANSPARENT);
      buffer->addChild(overlay);
      addChild(buffer);
    }
    // Draw dynamic length counter display
    {
      TransportDisplay *overlay = new TransportDisplay(HP);
      overlay->module = module;
      addChild(overlay);
    }
  }

  void appendContextMenu(Menu *menu) override {
    Transport *module = dynamic_cast<Transport *>(this->module);
    assert(module);

    menu->addChild(new MenuSeparator());
    MenuLabel *settingsLabel = new MenuLabel();
    settingsLabel->text = "Settings";
    menu->addChild(settingsLabel);

    struct OnStartValueItem : MenuItem {
      Transport *module;
      int value;
      void onAction(const event::Action &e) override {
        module->onStartActions = value;
      }
    };
    struct OnStartItem : MenuItem {
      Transport *module;
      void createOnStartSelection(Menu *menu, Transport *module,
                                      std::string label, int value) {
        OnStartValueItem *item = createMenuItem<OnStartValueItem>(
            label, CHECKMARK(module->onStartActions == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createOnStartSelection(menu, module, "No action", Transport::ON_START_NO_ACTION);
        createOnStartSelection(menu, module, "Send Reset", Transport::ON_START_RESET);
        return menu;
      }
    };
    OnStartItem *onStartActionsItem =
        createMenuItem<OnStartItem>("On Start", RIGHT_ARROW);
    onStartActionsItem->module = module;
    menu->addChild(onStartActionsItem);

    struct OnStopValueItem : MenuItem {
      Transport *module;
      int value;
      void onAction(const event::Action &e) override {
        module->onStopActions = value;
      }
    };
    struct OnStopItem : MenuItem {
      Transport *module;
      void createOnStopSelection(Menu *menu, Transport *module,
                                      std::string label, int value) {
        OnStopValueItem *item = createMenuItem<OnStopValueItem>(
            label, CHECKMARK(module->onStopActions == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createOnStopSelection(menu, module, "No action", Transport::ON_START_NO_ACTION);
        createOnStopSelection(menu, module, "Send Reset", Transport::ON_START_RESET);
        return menu;
      }
    };
    OnStopItem *onStopActionsItem =
        createMenuItem<OnStopItem>("On Stop", RIGHT_ARROW);
    onStopActionsItem->module = module;
    menu->addChild(onStopActionsItem);

    struct ClockDividerValueItem : MenuItem {
      Transport *module;
      int value;
      void onAction(const event::Action &e) override {
        module->clockDivider = value;
      }
    };
    struct ClockDividerItem : MenuItem {
      Transport *module;
      void createClockDividerSelection(Menu *menu, Transport *module,
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
        createClockDividerSelection(menu, module, "24 PPQN (MIDI-CV CLK)", 24 * 4);
        createClockDividerSelection(menu, module, "48 PPQN", 48 * 4);
        return menu;
      }
    };
    ClockDividerItem *clockDividerItem =
        createMenuItem<ClockDividerItem>("Incoming Clock Divider", RIGHT_ARROW);
    clockDividerItem->module = module;
    menu->addChild(clockDividerItem);


    struct QuantizeArmValueItem : MenuItem {
      Transport *module;
      int value;
      void onAction(const event::Action &e) override {
        module->armQuantize = value;
      }
    };
    struct QuantizeArmItem : MenuItem {
      Transport *module;
      void createQuantizeArmSelection(Menu *menu, Transport *module,
                                      std::string label, int value) {
        QuantizeArmValueItem *item = createMenuItem<QuantizeArmValueItem>(
            label, CHECKMARK(module->armQuantize == value));
        item->module = module;
        item->value = value;
        menu->addChild(item);
      }
      Menu *createChildMenu() override {
        Menu *menu = new Menu;
        createQuantizeArmSelection(menu, module, "OFF", 0);
        createQuantizeArmSelection(menu, module, "1", 1);
        createQuantizeArmSelection(menu, module, "2", 2);
        createQuantizeArmSelection(menu, module, "4", 4);
        createQuantizeArmSelection(menu, module, "8", 8);
        createQuantizeArmSelection(menu, module, "16", 16);
        createQuantizeArmSelection(menu, module, "32", 32);
        createQuantizeArmSelection(menu, module, "64", 64);
        createQuantizeArmSelection(menu, module, "128", 128);
        return menu;
      }
    };
    QuantizeArmItem *quantizeArmItem =
        createMenuItem<QuantizeArmItem>("Quantize Arming", RIGHT_ARROW);
    quantizeArmItem->module = module;
    menu->addChild(quantizeArmItem);

    struct StopOnRecordLengthItem : MenuItem {
      Transport *module;
      void onAction(const event::Action &e) override {
        module->recordLengthIsPlayLength = !module->recordLengthIsPlayLength;
      }
    };
    StopOnRecordLengthItem *stopOnRecordLengthItem =
        createMenuItem<StopOnRecordLengthItem>(
            "Stop after record length?",
            CHECKMARK(module->recordLengthIsPlayLength));
    stopOnRecordLengthItem->module = module;
    menu->addChild(stopOnRecordLengthItem);

    struct PlayIsIdempotentItem : MenuItem {
      Transport *module;
      void onAction(const event::Action &e) override {
        module->playIsIdempotent = !module->playIsIdempotent;
      }
    };
    PlayIsIdempotentItem *playIsIdempotentItem =
        createMenuItem<PlayIsIdempotentItem>(
            "Play button is toggle?",
            CHECKMARK(!module->playIsIdempotent));
    playIsIdempotentItem->module = module;
    menu->addChild(playIsIdempotentItem);

  }
};

Model *modelTransport = createModel<Transport, TransportWidget>("Transport");
