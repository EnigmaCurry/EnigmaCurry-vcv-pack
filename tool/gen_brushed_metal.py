#!/usr/bin/env python3
import numpy as np
from PIL import Image
from pathlib import Path

W, H = 512, 1024
SEED = 0xC0FFEE

OUT = Path(__file__).resolve().parents[1] / "res" / "textures" / "brushed_metal.png"


def blur1d_wrap(v: np.ndarray, k: int) -> np.ndarray:
    """1D box blur with wraparound."""
    if k <= 1:
        return v.astype(np.float32, copy=True)
    pad = k // 2
    padded = np.concatenate([v[-pad:], v, v[:pad]])
    kernel = np.ones(k, dtype=np.float32) / k
    return np.convolve(padded, kernel, mode="valid")[: len(v)]


def horizontal_blur_wrap(arr: np.ndarray, k: int) -> np.ndarray:
    """1D box blur along x with wraparound."""
    if k <= 1:
        return arr.astype(np.float32, copy=True)
    pad = k // 2
    padded = np.concatenate([arr[:, -pad:], arr, arr[:, :pad]], axis=1)
    kernel = np.ones(k, dtype=np.float32) / k
    out = np.empty_like(arr, dtype=np.float32)
    for y in range(arr.shape[0]):
        out[y] = np.convolve(padded[y], kernel, mode="valid")[: arr.shape[1]]
    return out


def vertical_blur_wrap(arr: np.ndarray, k: int) -> np.ndarray:
    """1D box blur along y with wraparound."""
    if k <= 1:
        return arr.astype(np.float32, copy=True)
    pad = k // 2
    padded = np.concatenate([arr[-pad:, :], arr, arr[:pad, :]], axis=0)
    kernel = np.ones(k, dtype=np.float32) / k
    out = np.empty_like(arr, dtype=np.float32)
    for x in range(arr.shape[1]):
        out[:, x] = np.convolve(padded[:, x], kernel, mode="valid")[: arr.shape[0]]
    return out


def normalize(a: np.ndarray) -> np.ndarray:
    a = a.astype(np.float32, copy=False)
    a -= a.mean()
    a /= (a.std() + 1e-9)
    return a


def main():
    rng = np.random.default_rng(SEED)

    # ------------------------------------------------------------------
    # 1) Main brush grain: fine horizontal lines
    #
    # Since brush lines run horizontally, we want a 1D signal over Y,
    # then replicate it across X.
    # ------------------------------------------------------------------
    row = rng.normal(0.0, 1.0, H).astype(np.float32)

    # Make it fine, not broad.
    row = blur1d_wrap(row, 1)

    # Remove low-frequency drift so it doesn't look like wood / banding.
    low = blur1d_wrap(row, 11)
    row = row - low * 0.92

    row = normalize(row)

    # Broadcast across width -> straight, horizontal hairlines
    brush = np.repeat(row[:, None], W, axis=1)

    # ------------------------------------------------------------------
    # 2) Very subtle irregularity so it doesn't look perfectly synthetic
    # ------------------------------------------------------------------
    micro = rng.normal(0.0, 1.0, (H, W)).astype(np.float32)

    # Tiny anisotropy, but not broad streaks
    micro = horizontal_blur_wrap(micro, 3)
    micro = vertical_blur_wrap(micro, 2)
    micro = normalize(micro)

    # ------------------------------------------------------------------
    # 3) Optional extremely faint machining variation
    # Keep this tiny. Too much = wood/plastic.
    # ------------------------------------------------------------------
    ultra_fine = rng.normal(0.0, 1.0, H).astype(np.float32)
    ultra_fine = blur1d_wrap(ultra_fine, 2)
    ultra_fine = normalize(ultra_fine)
    ultra_fine = np.repeat(ultra_fine[:, None], W, axis=1)

    # ------------------------------------------------------------------
    # 4) Combine at low contrast
    # ------------------------------------------------------------------
    texture = (
        brush * 1.00 +
        ultra_fine * 0.35 +
        micro * 0.10
    )

    texture = normalize(texture)

    # Very restrained luminance range
    delta = np.clip(texture * 1.6, -6.0, 6.0)

    # ------------------------------------------------------------------
    # 5) Dark neutral anodized-metal base
    # Avoid strong blue tint.
    # ------------------------------------------------------------------
    base_r, base_g, base_b = 46, 47, 49

    r = np.clip(base_r + delta, 0, 255).astype(np.uint8)
    g = np.clip(base_g + delta, 0, 255).astype(np.uint8)
    b = np.clip(base_b + delta * 1.02, 0, 255).astype(np.uint8)

    img = Image.fromarray(np.stack([r, g, b], axis=-1), "RGB")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT, optimize=True)

    size_kb = OUT.stat().st_size / 1024
    print(f"wrote {OUT} ({W}x{H}, {size_kb:.1f} KB)")


if __name__ == "__main__":
    main()
