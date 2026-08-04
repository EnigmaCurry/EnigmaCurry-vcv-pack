// BrushedTest — a wide, empty module used to eyeball the brushed-metal
// panel texture at HP counts far beyond any real module in the pack.
//
// The panel is BrushedMetalPanelTextured (same class Transport uses); the
// only thing we're inspecting is horizontal tileability and overall grain
// density on a very wide surface.

#include "components.hpp"
#include "plugin.hpp"

struct BrushedTest : Module {
  enum ParamIds { NUM_PARAMS };
  enum InputIds { NUM_INPUTS };
  enum OutputIds { NUM_OUTPUTS };
  enum LightIds { NUM_LIGHTS };

  BrushedTest() { config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS); }
  void process(const ProcessArgs &args) override {}
};

#define HP 60

struct BrushedTestWidget : ModuleWidget {
  BrushedTestWidget(BrushedTest *module) {
    setModule(module);
    setPanel(new BrushedMetalPanelTextured(HP));

    // Single centred label so the panel isn't visually empty — makes it
    // easier to spot tile seams if any exist.
    FramebufferWidget *buffer = new FramebufferWidget();
    DynamicOverlay *overlay = new DynamicOverlay(HP);
    overlay->addText("BrushedTest 60HP", 20,
                     Vec(mm2px(HP * HP_UNIT / 2), 25), WHITE, CLEAR, MANROPE);
    buffer->addChild(overlay);
    addChild(buffer);
  }
};

Model *modelBrushedTest = createModel<BrushedTest, BrushedTestWidget>("BrushedTest");
