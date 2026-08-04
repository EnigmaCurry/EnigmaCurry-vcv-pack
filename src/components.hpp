/*
 * EnigmaCurry-vcv-pack
 * Copyright (C) 2021-2022 EnigmaCurry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the /LICENSE file.
 */
#include "plugin.hpp"
#include "rack.hpp"
#include <iostream>
#include <random>
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

// Procedural gunmetal brushed-steel panel. Sized purely from HP; no artwork.
// Wrapped in a FramebufferWidget so this is rasterized once and cached.
//
// Layered back-to-front:
//   1. Base linear gradient (dark gunmetal)
//   2. Specular sheen — soft horizontal bright band, aligned across all
//      panels so the reflection reads as one light source hitting the rack
//   3. Fine brushed grain — shared seed → grain lines up across panels,
//      giving the "one sheet of metal" illusion
//   4. Edge shading (subtle top highlight + bottom shadow)
//   5. Outer border
struct BrushedMetalDraw : rack::TransparentWidget {
  int hp_width;
  BrushedMetalDraw(int hp) : hp_width(hp) {
    box.size = rack::mm2px(rack::Vec(hp * HP_UNIT, HEIGHT));
  }
  void draw(const DrawArgs &args) override {
    float w = box.size.x;
    float h = box.size.y;

    // 1. Base gradient — narrow range so the sheen and grain read on top.
    NVGpaint bg = nvgLinearGradient(args.vg, 0, 0, 0, h,
                                    nvgRGB(0x2e, 0x32, 0x36),
                                    nvgRGB(0x1e, 0x21, 0x23));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, w, h);
    nvgFillPaint(args.vg, bg);
    nvgFill(args.vg);

    // 2. Specular sheen band — anisotropic reflection off horizontal grain.
    //    Band centre at a fixed y-fraction so every panel's sheen lines up
    //    with its neighbours, reading as a single light source across the
    //    whole rack row.
    float bc = h * 0.30f;
    float br = h * 0.34f;
    NVGpaint sheenUp = nvgLinearGradient(args.vg, 0, bc - br, 0, bc,
                                         nvgRGBA(0xff, 0xff, 0xff, 0x00),
                                         nvgRGBA(0xff, 0xff, 0xff, 0x24));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, bc - br, w, br);
    nvgFillPaint(args.vg, sheenUp);
    nvgFill(args.vg);
    NVGpaint sheenDn = nvgLinearGradient(args.vg, 0, bc, 0, bc + br,
                                         nvgRGBA(0xff, 0xff, 0xff, 0x24),
                                         nvgRGBA(0xff, 0xff, 0xff, 0x00));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, bc, w, br);
    nvgFillPaint(args.vg, sheenDn);
    nvgFill(args.vg);

    // 3. Fine horizontal grain — shared seed keeps lines aligned across
    //    every panel (continuous-sheet illusion).
    {
      std::mt19937 rng(0xC0FFEEu);
      std::uniform_real_distribution<float> y_dist(0.f, h);
      std::uniform_int_distribution<int> a_dist(3, 16);
      std::uniform_int_distribution<int> tone(0, 1);
      int line_count = (int)(h * 2.0f);
      nvgLineCap(args.vg, NVG_BUTT);
      nvgStrokeWidth(args.vg, 1.f);
      for (int i = 0; i < line_count; i++) {
        float y = y_dist(rng);
        int a = a_dist(rng);
        NVGcolor c = tone(rng) ? nvgRGBA(0xff, 0xff, 0xff, a)
                               : nvgRGBA(0x00, 0x00, 0x00, a);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0, y);
        nvgLineTo(args.vg, w, y);
        nvgStrokeColor(args.vg, c);
        nvgStroke(args.vg);
      }
    }

    // 4. Edge shading — subtle top highlight + bottom shadow.
    NVGpaint topShade = nvgLinearGradient(args.vg, 0, 0, 0, 6,
                                          nvgRGBA(0xff, 0xff, 0xff, 0x1e),
                                          nvgRGBA(0xff, 0xff, 0xff, 0x00));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, w, 6);
    nvgFillPaint(args.vg, topShade);
    nvgFill(args.vg);

    NVGpaint botShade = nvgLinearGradient(args.vg, 0, h - 10, 0, h,
                                          nvgRGBA(0x00, 0x00, 0x00, 0x00),
                                          nvgRGBA(0x00, 0x00, 0x00, 0x40));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, h - 10, w, 10);
    nvgFillPaint(args.vg, botShade);
    nvgFill(args.vg);

    // 5. Thin outer border — grounds the panel against neighbours.
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0.5f, 0.5f, w - 1.f, h - 1.f);
    nvgStrokeColor(args.vg, nvgRGBA(0x00, 0x00, 0x00, 0x90));
    nvgStrokeWidth(args.vg, 1.f);
    nvgStroke(args.vg);
  }
};

struct BrushedMetalPanel : rack::FramebufferWidget {
  BrushedMetalPanel(int hp_width) {
    box.size = rack::mm2px(rack::Vec(hp_width * HP_UNIT, HEIGHT));
    BrushedMetalDraw *d = new BrushedMetalDraw(hp_width);
    addChild(d);
  }
};

// Photographic-brushed-aluminium variant. Replaces steps 1 (base gradient) +
// 3 (procedural grain) of BrushedMetalDraw with a single tiled blit of a
// pre-baked texture; keeps the sheen / edges / border on top.
//
// Not FBO-cached: the draw is now one image fill plus a handful of gradient
// rects, which is cheaper than the procedural grain's ~1000 stroke calls, so
// there is nothing to amortise.
//
// Texture is lazy-loaded on first draw so args.vg is the correct NanoVG
// context for the created image handle (same pattern as CardinalBlankImage).
struct BrushedMetalPanelTextured : rack::TransparentWidget {
  int hp_width;
  std::shared_ptr<rack::window::Image> tex;
  int tex_w = 0, tex_h = 0;

  BrushedMetalPanelTextured(int hp) : hp_width(hp) {
    box.size = rack::mm2px(rack::Vec(hp * HP_UNIT, HEIGHT));
  }

  void draw(const DrawArgs &args) override {
    float w = box.size.x;
    float h = box.size.y;

    // 1 + 3. Brushed metal texture, scaled so its height matches the panel
    //        and tiled horizontally. UVs anchored at (0,0) in panel space —
    //        cross-panel grain continuity is a follow-up.
    if (!tex) {
      tex = APP->window->loadImage(
          rack::asset::plugin(pluginInstance, "res/textures/brushed_metal.png"));
      if (tex)
        nvgImageSize(args.vg, tex->handle, &tex_w, &tex_h);
    }
    if (tex && tex_h > 0) {
      float sx = h / (float)tex_h;
      float paint_w = tex_w * sx;
      NVGpaint p =
          nvgImagePattern(args.vg, 0.f, 0.f, paint_w, h, 0.f, tex->handle, 1.f);
      nvgBeginPath(args.vg);
      nvgRect(args.vg, 0.f, 0.f, w, h);
      nvgFillPaint(args.vg, p);
      nvgFill(args.vg);
    }

    // 2. Specular sheen band (aligned across panels, same as procedural).
    float bc = h * 0.30f;
    float br = h * 0.34f;
    NVGpaint sheenUp = nvgLinearGradient(args.vg, 0, bc - br, 0, bc,
                                         nvgRGBA(0xff, 0xff, 0xff, 0x00),
                                         nvgRGBA(0xff, 0xff, 0xff, 0x24));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, bc - br, w, br);
    nvgFillPaint(args.vg, sheenUp);
    nvgFill(args.vg);
    NVGpaint sheenDn = nvgLinearGradient(args.vg, 0, bc, 0, bc + br,
                                         nvgRGBA(0xff, 0xff, 0xff, 0x24),
                                         nvgRGBA(0xff, 0xff, 0xff, 0x00));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, bc, w, br);
    nvgFillPaint(args.vg, sheenDn);
    nvgFill(args.vg);

    // 4. Edge shading — top highlight + bottom shadow.
    NVGpaint topShade = nvgLinearGradient(args.vg, 0, 0, 0, 6,
                                          nvgRGBA(0xff, 0xff, 0xff, 0x1e),
                                          nvgRGBA(0xff, 0xff, 0xff, 0x00));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, w, 6);
    nvgFillPaint(args.vg, topShade);
    nvgFill(args.vg);

    NVGpaint botShade = nvgLinearGradient(args.vg, 0, h - 10, 0, h,
                                          nvgRGBA(0x00, 0x00, 0x00, 0x00),
                                          nvgRGBA(0x00, 0x00, 0x00, 0x40));
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, h - 10, w, 10);
    nvgFillPaint(args.vg, botShade);
    nvgFill(args.vg);

    // 5. Thin outer border.
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0.5f, 0.5f, w - 1.f, h - 1.f);
    nvgStrokeColor(args.vg, nvgRGBA(0x00, 0x00, 0x00, 0x90));
    nvgStrokeWidth(args.vg, 1.f);
    nvgStroke(args.vg);
  }
};
