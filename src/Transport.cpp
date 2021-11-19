/*
  DAW-like transport play/stop/record for X bars
*/

#include "components.hpp"
#include "plugin.hpp"
#include <string>

#define TRIGGER_DURATION 1e-3f

struct Transport : Module {
  enum ParamIds { LEN, TAP_LEN, TAP_PLAY, TAP_ARM, NUM_PARAMS };
  enum InputIds { PLAY, ARM, CLK, NUM_INPUTS };
  enum OutputIds { PGAT, PTRG, RST, RGAT, RTRG, NUM_OUTPUTS };
  enum LightIds { TAP_LEN_LIGHT, TAP_PLAY_LIGHT, TAP_ARM_LIGHT, NUM_LIGHTS };
  double recordLength;
  bool bypassRecordLength;
  int playCount;
  bool playing;
  bool armed;
  int armQuantize;
  bool toFlipArm;
  float blinkPhase = 0.f;
  dsp::SchmittTrigger clockTrigger, playTrigger, armTrigger;
  dsp::BooleanTrigger tapLenTrigger, tapPlayTrigger, tapArmTrigger;
  dsp::PulseGenerator playPulse, recordPulse, resetPulse;

  Transport() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configParam(Transport::LEN, 0, 256, 32, "Record Length");
    configParam(TAP_LEN, 0.f, 1.f, 0.f, "Bypass record length");
    configParam(TAP_PLAY, 0.f, 1.f, 0.f, "Tap to play");
    configParam(TAP_ARM, 0.f, 1.f, 0.f, "Tap to arm record");
    reset(1);
  }

  void process(const ProcessArgs &args) override {
    recordLength = int(params[LEN].getValue());

    if (tapLenTrigger.process(params[TAP_LEN].getValue())) {
      bypassRecordLength = !bypassRecordLength;
    }
    // PLAY
    if (playTrigger.process(inputs[PLAY].getNormalVoltage(0.0)) ||
        tapPlayTrigger.process(params[TAP_PLAY].getValue())) {
      playing = !playing;
      if (!playing) {
        resetPulse.trigger(TRIGGER_DURATION);
        reset();
      }
      playPulse.trigger(TRIGGER_DURATION);
      if (armed)
        recordPulse.trigger(TRIGGER_DURATION);
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
      if (bypassRecordLength || recordLength == 0 || playCount < recordLength) {
        playCount++;
      } else {
        resetPulse.trigger(TRIGGER_DURATION);
        reset();
      }
      if (toFlipArm) {
        if (armQuantize <= 0 || playCount % armQuantize == 0) {
          armed = !armed;
          toFlipArm = false;
          recordPulse.trigger(TRIGGER_DURATION);
        }
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
  }

  void reset(bool init) {
    playCount = 0;
    if (playing) {
      playPulse.trigger(TRIGGER_DURATION);
      resetPulse.trigger(TRIGGER_DURATION);
    }
    playing = false;

    if (armed)
      recordPulse.trigger(TRIGGER_DURATION);
    armed = false;
    toFlipArm = false;
    if (init) {
      armQuantize = 0;
      bypassRecordLength = true;
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
    return rootJ;
  }

  void dataFromJson(json_t *rootJ) override {
    json_t *bypassJ = json_object_get(rootJ, "bypassRecordLength");
    if (bypassJ)
      bypassRecordLength = (bool)json_integer_value(bypassJ);

    json_t *armedJ = json_object_get(rootJ, "armed");
    if (armedJ)
      armed = (bool)json_integer_value(armedJ);
    json_t *playingJ = json_object_get(rootJ, "playing");
    if (armedJ)
      playing = (bool)json_integer_value(playingJ);
  }
};

NVGcolor RED = nvgRGBA(0xff, 0x00, 0x00, 0xff);
NVGcolor GREEN = nvgRGBA(0x00, 0xff, 0x00, 0xff);
NVGcolor WHITE = nvgRGBA(0xff, 0xff, 0xff, 0xff);
NVGcolor RED_TRANSPARENT = nvgRGBA(0xff, 0x00, 0x22, 0x50);
NVGcolor BLACK = nvgRGBA(0x00, 0x00, 0x00, 0xff);
NVGcolor BLACK_TRANSPARENT = nvgRGBA(0x00, 0x00, 0x00, 0x77);
NVGcolor CLEAR = nvgRGBA(0x00, 0x00, 0x00, 0x00);

#define HP 10
#define ROWS 24
#define COLUMNS 10
panel_grid<HP, ROWS, COLUMNS> grid;

struct TransportDisplay : public DynamicOverlay {
  using DynamicOverlay::DynamicOverlay;
  Transport *module;

  TransportDisplay(int hp_width) : DynamicOverlay(hp_width) {}

  std::string pad(int num) {
    if (num > 999)
      return "XXX";
    else if (num == 0)
      return "";
    std::string s = std::to_string(num);
    if (s.length() < 3) {
      s.insert(s.begin(), 3 - s.length(), '_');
    }
    return s;
  }

  void draw(const DrawArgs &args) override {
    if (!module)
      return;
    DynamicOverlay::clear();
    std::string recordLength = pad(module->recordLength);
    std::string playCount = pad(module->playCount);

    addText(recordLength, 25, grid.loc(4, 7).minus(Vec(8, -15)),
            (!module->bypassRecordLength && module->recordLength > 0) ? RED
                                                                      : WHITE,
            CLEAR, DSEG);
    addText(playCount, 25, grid.loc(8, 7).minus(Vec(8, -15)), GREEN, CLEAR,
            DSEG);
    DynamicOverlay::draw(args);
  }
};

struct TransportWidget : ModuleWidget {
  TransportWidget(Transport *module) {
    setModule(module);
    setPanel(
        APP->window->loadSvg(asset::plugin(pluginInstance, "res/10hp.svg")));

    Vec lenLoc = grid.loc(4, 1);
    Vec tapLenLoc = grid.loc(4, 3);
    Vec clkLoc = grid.loc(8, 1);
    Vec rstLoc = grid.loc(8, 3);
    Vec playLoc = grid.loc(12, 1);
    Vec tapPlayLoc = grid.loc(12, 3);
    Vec pgatLoc = grid.loc(12, 6);
    Vec ptrgLoc = grid.loc(12, 8);
    Vec armRecLoc = grid.loc(16, 1);
    Vec tapArmLoc = grid.loc(16, 3);
    Vec rgatLoc = grid.loc(16, 6);
    Vec rtrgLoc = grid.loc(16, 8);
    Vec lengthDisplayLoc = grid.loc(4, 7);
    Vec playLengthDisplayLoc = grid.loc(8, 7);

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

    addInput(createInputCentered<PJ301MPort>(clkLoc, module, Transport::CLK));
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
  }
};

Model *modelTransport = createModel<Transport, TransportWidget>("Transport");
