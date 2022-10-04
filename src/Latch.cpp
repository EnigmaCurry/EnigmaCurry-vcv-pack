/*
  Latches with discrete triggers and resets.
*/

#include "components.hpp"
#include "plugin.hpp"
#include <string>

#define HYSTERISYS_SAMPLES 1

struct Latch : Module {
  enum ParamIds { NUM_PARAMS };
  enum InputIds { TRIG1, RESET1, TRIG2, RESET2, NUM_INPUTS };
  enum OutputIds { LATCH1, LATCH2, NUM_OUTPUTS };
  enum LightIds { NUM_LIGHTS };
  bool latch1Engaged[16] = {false};
  bool latch2Engaged[16] = {false};
  int latch1HysterisysSamples[16] = {HYSTERISYS_SAMPLES};
  int latch2HysterisysSamples[16] = {HYSTERISYS_SAMPLES};

  Latch() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
    configInput(TRIG1, "TRIG 1");
    configInput(RESET1, "RESET 1");
    configOutput(LATCH1, "LATCH 1");
    configInput(TRIG2, "TRIG 2");
    configInput(RESET2, "RESET 2");
    configOutput(LATCH2, "LATCH 2");
  }

  void processLatch(int latchHysterisysSamples[], bool latchesEngaged[], InputIds trig, InputIds reset,  OutputIds latch) {
    int channels = std::max({1, inputs[trig].getChannels(), inputs[reset].getChannels()});
    for (int c = 0; c < channels; c++) {
      if (inputs[reset].getPolyVoltage(c) >= 1.f) {
        latchesEngaged[c] = false;
        latchHysterisysSamples[c] = 0;
      } else if (latchHysterisysSamples[c] < HYSTERISYS_SAMPLES) {
        latchHysterisysSamples[c] += 1;
        continue;
      } else if (!latchesEngaged[c] && inputs[trig].getPolyVoltage(c) >= 1.f) {
        latchesEngaged[c] = true;
        latchHysterisysSamples[c] = 0;
      }
      outputs[latch].setVoltage(latchesEngaged[c] ? 10.f : 0.f, c);
    }
    outputs[latch].setChannels(channels);
  }

  void process(const ProcessArgs &args) override {
    processLatch(latch1HysterisysSamples, latch1Engaged, TRIG1, RESET1, LATCH1);
    processLatch(latch2HysterisysSamples,latch2Engaged, TRIG2, RESET2, LATCH2);
  }
};


#define HP 3
#define ROWS 14
#define COLUMNS 1
panel_grid<HP, ROWS, COLUMNS> latchGrid;


struct LatchWidget : ModuleWidget {
  LatchWidget(Latch *module) {
    setModule(module);
    setPanel(
             APP->window->loadSvg(asset::plugin(pluginInstance, "res/3hp.svg")));

    Vec trig1Loc = latchGrid.loc(2, 0);
    Vec reset1Loc = latchGrid.loc(4, 0);
    Vec latch1Loc = latchGrid.loc(6, 0);
    Vec trig2Loc = latchGrid.loc(8, 0);
    Vec reset2Loc = latchGrid.loc(10, 0);
    Vec latch2Loc = latchGrid.loc(12, 0);

    addInput(createInputCentered<PJ301MPort>(trig1Loc, module, Latch::TRIG1));
    addInput(createInputCentered<PJ301MPort>(reset1Loc, module, Latch::RESET1));
    addOutput(createOutputCentered<PJ301MPort>(latch1Loc, module, Latch::LATCH1));
    addInput(createInputCentered<PJ301MPort>(trig2Loc, module, Latch::TRIG2));
    addInput(createInputCentered<PJ301MPort>(reset2Loc, module, Latch::RESET2));
    addOutput(createOutputCentered<PJ301MPort>(latch2Loc, module, Latch::LATCH2));

    // Draw static text labels to frame buffer
    {
      FramebufferWidget *buffer = new FramebufferWidget();
      DynamicOverlay *overlay = new DynamicOverlay(HP);
      overlay->addText("Latch", 20, Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE,
                       CLEAR, MANROPE);
      overlay->addText("TRIG", 13, trig1Loc.minus(Vec(0, 17)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("RESET", 13, reset1Loc.minus(Vec(0, 17)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("LATCH", 13, latch1Loc.minus(Vec(0, 17)), WHITE,
                       BLACK_TRANSPARENT);
      overlay->addText("TRIG", 13, trig2Loc.minus(Vec(0, 17)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("RESET", 13, reset2Loc.minus(Vec(0, 17)), WHITE,
                       RED_TRANSPARENT);
      overlay->addText("LATCH", 13, latch2Loc.minus(Vec(0, 17)), WHITE,
                       BLACK_TRANSPARENT);

      buffer->addChild(overlay);
      addChild(buffer);
    }
  }
};

Model *modelLatch = createModel<Latch, LatchWidget>("Latch");

