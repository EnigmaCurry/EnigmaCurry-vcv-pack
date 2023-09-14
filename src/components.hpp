/*
 * EnigmaCurry-vcv-pack
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
#include "plugin.hpp"
#include "rack.hpp"
#include <iostream>
#include <vector>

#define HEIGHT 128.5
#define HP_UNIT 5.08

extern NVGcolor RED;
extern NVGcolor GREEN;
extern NVGcolor WHITE;
extern NVGcolor RED_TRANSPARENT;
extern NVGcolor BLACK;
extern NVGcolor BLACK_TRANSPARENT;
extern NVGcolor CLEAR;
extern NVGcolor PURPLE;
extern NVGcolor BLUE;
extern NVGcolor YELLOW;
extern NVGcolor PINK;

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
  float padding;
} draw_text;

typedef struct {
  rack::Vec start;
  rack::Vec end;
  NVGcolor color;
  int roundness;
} draw_box;

struct DynamicOverlay : rack::TransparentWidget {
  ModuleWidget *module;
  std::vector<draw_text> text_calls;
  std::vector<draw_box> box_calls;

  DynamicOverlay(int hp_width) {
    box.pos = Vec(0, 0);
    box.size = mm2px(Vec(hp_width * HP_UNIT, HEIGHT));
  }
  void addText(std::string text, int size, rack::Vec px, NVGcolor color,
               NVGcolor bgColor, int font, float padding) {
    if (text.length())
      text_calls.push_back({text, size, px, color, font, bgColor, padding});
  }
  void addText(std::string text, int size, rack::Vec px, NVGcolor color,
               NVGcolor bgColor, int font) {
    addText(text, size, px, color, bgColor, font, 1);
  }
  void addText(std::string text, int size, rack::Vec px, NVGcolor color,
               NVGcolor bgColor) {
    addText(text, size, px, color, bgColor, FANTASQUE, 1);
  }
  void addBox(rack::Vec start, rack::Vec end, NVGcolor color, int roundness) {
    box_calls.push_back({start, end, color, roundness});
  }
  void drawText(const DrawArgs &args, draw_text dt) {
    std::shared_ptr<Font> font;
    switch (dt.font) {
    case MANROPE:
      font = APP->window->loadFont(
          asset::plugin(pluginInstance, "res/fonts/manrope/Manrope-Regular.ttf"));
      break;
    case DSEG:
      font = APP->window->loadFont(asset::plugin(
          pluginInstance, "res/fonts/dseg/DSEG14Modern-Regular.ttf"));
      break;
    case FANTASQUE:
      font = APP->window->loadFont(asset::plugin(
          pluginInstance, "res/fonts/Fantasque/FantasqueSansMono-Regular.ttf"));
      break;
    default:
      font = APP->window->loadFont(asset::plugin(
          pluginInstance, "res/fonts/Fantasque/FantasqueSansMono-Regular.ttf"));
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
      if (dt.padding < 1) {
        nvgRoundedRect(args.vg, (int)bounds[0] - 1 - x_offset, (int)bounds[1],
                       (int)(bounds[2] - 1 - bounds[0]) + 5,
                       (int)(bounds[3] - 4 - bounds[1]) + 5,
                       ((int)(bounds[3] - bounds[1]) - 1) / 2 - 1);
      } else {
        nvgRoundedRect(args.vg, (int)bounds[0] - 2 - x_offset, (int)bounds[1] - 2,
                       (int)(bounds[2] - bounds[0]) + 5,
                       (int)(bounds[3] - bounds[1]) + 5,
                       ((int)(bounds[3] - bounds[1]) - 1) / 2 - 1);
      }
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
    nvgRoundedRect(args.vg, db.start.x, db.start.y, db.end.x, db.end.y, db.roundness);
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

std::string padTripleDigits(int num, int wide);
std::string padTripleDigits(int num);
