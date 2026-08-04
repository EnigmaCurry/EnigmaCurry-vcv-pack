// Procedural brushed-aluminium texture. See brushed_metal.hpp header for
// the algorithm's provenance and usage.
//
// Pipeline (mirrors tool/gen_brushed_metal.py, now removed):
//   1. Per-row 1D Gaussian noise + high-pass (subtract wrap-blur) → the
//      dominant "hairline" grain, broadcast across every X.
//   2. Very short-radius 2D noise blurred H then V → anti-synthetic
//      irregularity so the hairlines aren't perfectly straight.
//   3. Another per-row 1D noise, tiny blur → ultra-fine machining variation.
//   4. Weighted sum → normalize → clip to a narrow luminance delta.
//   5. Dark neutral gunmetal base + delta per channel, blue slightly boosted.

#include "brushed_metal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace brushed_metal {
namespace {

constexpr uint32_t SEED = 0xC0FFEEu;

// One handle shared across all EnigmaCurry panels. -1 = not yet built.
// cachedVg is invalidated if Rack ever hands us a different context.
int cachedHandle = -1;
NVGcontext *cachedVg = nullptr;

// 1D box blur with wrap-around indexing.
void blur1dWrap(std::vector<float> &v, int k) {
  if (k <= 1)
    return;
  int n = (int)v.size();
  std::vector<float> out(n);
  int pad = k / 2;
  float inv_k = 1.f / (float)k;
  for (int i = 0; i < n; i++) {
    float s = 0.f;
    for (int j = -pad; j < k - pad; j++) {
      int idx = i + j;
      if (idx < 0)
        idx += n;
      else if (idx >= n)
        idx -= n;
      s += v[idx];
    }
    out[i] = s * inv_k;
  }
  v = std::move(out);
}

// 2D horizontal blur (wrap-around x).
void hBlurWrap(std::vector<float> &arr, int w, int h, int k) {
  if (k <= 1)
    return;
  std::vector<float> out(arr.size());
  int pad = k / 2;
  float inv_k = 1.f / (float)k;
  for (int y = 0; y < h; y++) {
    const float *row = &arr[(size_t)y * w];
    float *orow = &out[(size_t)y * w];
    for (int x = 0; x < w; x++) {
      float s = 0.f;
      for (int j = -pad; j < k - pad; j++) {
        int xi = x + j;
        if (xi < 0)
          xi += w;
        else if (xi >= w)
          xi -= w;
        s += row[xi];
      }
      orow[x] = s * inv_k;
    }
  }
  arr = std::move(out);
}

// 2D vertical blur (wrap-around y).
void vBlurWrap(std::vector<float> &arr, int w, int h, int k) {
  if (k <= 1)
    return;
  std::vector<float> out(arr.size());
  int pad = k / 2;
  float inv_k = 1.f / (float)k;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float s = 0.f;
      for (int j = -pad; j < k - pad; j++) {
        int yi = y + j;
        if (yi < 0)
          yi += h;
        else if (yi >= h)
          yi -= h;
        s += arr[(size_t)yi * w + x];
      }
      out[(size_t)y * w + x] = s * inv_k;
    }
  }
  arr = std::move(out);
}

void normalize(std::vector<float> &v) {
  double sum = 0.0;
  for (float x : v)
    sum += x;
  float mean = (float)(sum / (double)v.size());
  double sqsum = 0.0;
  for (float x : v) {
    double d = (double)x - mean;
    sqsum += d * d;
  }
  float stdev = (float)std::sqrt(sqsum / (double)v.size()) + 1e-9f;
  for (float &x : v)
    x = (x - mean) / stdev;
}

std::vector<uint8_t> generate() {
  std::mt19937 rng(SEED);
  std::normal_distribution<float> gauss(0.f, 1.f);
  auto sample_normal = [&](std::vector<float> &v) {
    for (float &x : v)
      x = gauss(rng);
  };

  // 1. Row grain — 1D noise minus its own low-pass, broadcast to columns.
  std::vector<float> row(H);
  sample_normal(row);
  std::vector<float> low = row;
  blur1dWrap(low, 11);
  for (int i = 0; i < H; i++)
    row[i] -= low[i] * 0.92f;
  normalize(row);

  // 2. Sub-hairline 2D irregularity.
  std::vector<float> micro((size_t)H * W);
  sample_normal(micro);
  hBlurWrap(micro, W, H, 3);
  vBlurWrap(micro, W, H, 2);
  normalize(micro);

  // 3. Ultra-fine per-row variation.
  std::vector<float> ultra(H);
  sample_normal(ultra);
  blur1dWrap(ultra, 2);
  normalize(ultra);

  // 4. Combine (weights straight from the Python reference), renormalize,
  //    clip to a restrained luminance delta.
  std::vector<float> tex((size_t)H * W);
  for (int y = 0; y < H; y++) {
    float b = row[y];
    float u = ultra[y];
    for (int x = 0; x < W; x++) {
      tex[(size_t)y * W + x] = b * 1.00f + u * 0.35f + micro[(size_t)y * W + x] * 0.10f;
    }
  }
  normalize(tex);

  // 5. Emit RGBA. Dark neutral gunmetal + delta; blue slightly boosted
  //    so highlights read as cool aluminium rather than dead grey.
  std::vector<uint8_t> rgba((size_t)H * W * 4);
  constexpr float base_r = 46.f;
  constexpr float base_g = 47.f;
  constexpr float base_b = 49.f;
  auto to_u8 = [](float v) -> uint8_t {
    return (uint8_t)std::max(0.f, std::min(255.f, v));
  };
  for (int i = 0; i < H * W; i++) {
    float d = std::max(-6.f, std::min(6.f, tex[i] * 1.6f));
    rgba[(size_t)i * 4 + 0] = to_u8(base_r + d);
    rgba[(size_t)i * 4 + 1] = to_u8(base_g + d);
    rgba[(size_t)i * 4 + 2] = to_u8(base_b + d * 1.02f);
    rgba[(size_t)i * 4 + 3] = 255;
  }
  return rgba;
}

}  // namespace

int getHandle(NVGcontext *vg) {
  if (cachedHandle >= 0 && cachedVg == vg)
    return cachedHandle;
  auto rgba = generate();
  cachedHandle = nvgCreateImageRGBA(vg, W, H, 0, rgba.data());
  cachedVg = vg;
  return cachedHandle;
}

}  // namespace brushed_metal
