#include "plugin.hpp"
#include "rack.hpp"
#include <iostream>
#include <vector>

#define HEIGHT 128.5
#define HP_UNIT 5.08

enum fonts { MANROPE, DSEG, FANTASQUE };

template <int HP, int ROWS, int COLUMNS> struct panel_grid {
  rack::Vec loc(int row, int col, double x_offset, double y_offset) {
    return rack::mm2px(
        rack::Vec((HP_UNIT * HP / COLUMNS) * col +
                      (0.5 * (HP_UNIT * HP / COLUMNS) + x_offset),
                  (HEIGHT / ROWS) * row + (0.5 * (HEIGHT / ROWS)) + y_offset));
  }
  rack::Vec loc(int row, int col, double x_y_offset) {
    panel_grid<HP, ROWS, COLUMNS> f;
    return f.loc(row, col, x_y_offset, x_y_offset);
  }
  rack::Vec loc(int row, int col) {
    panel_grid<HP, ROWS, COLUMNS> f;
    return f.loc(row, col, 0, 0);
  }
  rack::Vec loc(rack::Vec pos, rack::Vec offset) {
    panel_grid<HP, ROWS, COLUMNS> f;
    return f.loc(pos.x, pos.y, offset.x, offset.y);
  }
  rack::Vec loc(rack::Vec pos) {
    panel_grid<HP, ROWS, COLUMNS> f;
    return f.loc(pos.x, pos.y, 0, 0);
  }
};

typedef struct {
  std::string text;
  int size;
  rack::Vec pos;
  NVGcolor color;
  int font;
  NVGcolor bgColor;
} draw_text;

typedef struct {
  rack::Vec start;
  rack::Vec end;
  NVGcolor color;
} draw_box;

struct DynamicOverlay : rack::TransparentWidget {
  ModuleWidget *module;
  std::shared_ptr<Font> fontManrope;
  std::shared_ptr<Font> fontManropeBold;
  std::shared_ptr<Font> fontDseg;
  std::shared_ptr<Font> fontFantasque;
  std::vector<draw_text> text_calls;
  std::vector<draw_box> box_calls;

  DynamicOverlay(int hp_width) {
    box.pos = Vec(0, 0);
    box.size = mm2px(Vec(hp_width * HP_UNIT, HEIGHT));
    fontManrope = APP->window->loadFont(
        asset::plugin(pluginInstance, "res/fonts/manrope/Manrope-Regular.ttf"));
    fontFantasque = APP->window->loadFont(asset::plugin(
        pluginInstance, "res/fonts/Fantasque/FantasqueSansMono-Regular.ttf"));
    fontDseg = APP->window->loadFont(asset::plugin(
        pluginInstance, "res/fonts/dseg/DSEG14Modern-Regular.ttf"));
  }
  void addText(std::string text, int size, rack::Vec px, NVGcolor color,
               NVGcolor bgColor, int font) {
    text_calls.push_back({text, size, px, color, font, bgColor});
  }
  void addText(std::string text, int size, rack::Vec px, NVGcolor color,
               NVGcolor bgColor) {
    addText(text, size, px, color, bgColor, FANTASQUE);
  }
  void addBox(rack::Vec start, rack::Vec end, NVGcolor color) {
    box_calls.push_back({start, end, color});
  }
  void drawText(const DrawArgs &args, draw_text dt) {
    std::shared_ptr<Font> font;
    switch (dt.font) {
    case MANROPE:
      font = fontManrope;
      break;
    case DSEG:
      font = fontDseg;
      break;
    case FANTASQUE:
      font = fontFantasque;
      break;
    default:
      font = fontFantasque;
    }
    nvgFontSize(args.vg, dt.size);
    nvgFontFaceId(args.vg, font->handle);
    nvgTextLetterSpacing(args.vg, 0);

    float bounds[4];
    nvgBeginPath(args.vg);
    nvgTextBounds(args.vg, dt.pos.x, dt.pos.y, dt.text.c_str(), NULL, bounds);
    float x_offset = 0.5 * (bounds[2] - bounds[0]);
    // draw background:
    if (dt.bgColor.a != 0) {
      nvgFillColor(args.vg, dt.bgColor);
      nvgRoundedRect(args.vg, (int)bounds[0] - 2 - x_offset, (int)bounds[1] - 2,
                     (int)(bounds[2] - bounds[0]) + 5,
                     (int)(bounds[3] - bounds[1]) + 5,
                     ((int)(bounds[3] - bounds[1]) - 1) / 2 - 1);
      nvgFill(args.vg);
    }
    // draw text:
    nvgBeginPath(args.vg);
    nvgFillColor(args.vg, dt.color);
    nvgText(args.vg, dt.pos.x - x_offset, dt.pos.y, dt.text.c_str(), NULL);
  }
  void drawBox(const DrawArgs &args, draw_box db) {
    nvgBeginPath(args.vg);
    nvgFillColor(args.vg, db.color);
    nvgRoundedRect(args.vg, db.start.x, db.start.y, db.end.x, db.end.y, 10);
    nvgFill(args.vg);
    nvgClosePath(args.vg);
  }
  void draw(const DrawArgs &args) override {
    for (draw_text dt : text_calls)
      drawText(args, dt);
    for (draw_box db : box_calls)
      drawBox(args, db);
  }
  void clear() {
    text_calls.clear();
    box_calls.clear();
  }
};
