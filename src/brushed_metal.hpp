// Procedurally-generated brushed-aluminium panel texture. Runtime-built
// once per Cardinal instance and cached; no shipped PNG, no PIL / numpy.
//
// Ported from tool/gen_brushed_metal.py (removed). C++ and Python produce
// different pixel values (different RNG streams and float-op order) but
// the same visual class of pattern.
//
// Usage from a widget's draw():
//   int handle = brushed_metal::getHandle(args.vg);
//   NVGpaint p = nvgImagePattern(args.vg, 0, 0, W, H, 0, handle, 1.f);
//
// The handle is cached per NVGcontext*; if Rack ever swaps contexts the
// texture is regenerated against the new one.
#pragma once
#include "rack.hpp"

namespace brushed_metal {

constexpr int W = 512;
constexpr int H = 1024;

// Returns a NanoVG image handle for the brushed-metal texture, generating
// and uploading it on first call (or when vg differs from the cache).
int getHandle(NVGcontext *vg);

}  // namespace brushed_metal
