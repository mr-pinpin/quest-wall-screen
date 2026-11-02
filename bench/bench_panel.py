#!/usr/bin/env python3
"""Offline benchmark for the panel display experiments (NATIVE, MIPQ), 2026-11-02.

Question: do NATIVE (stream the laptop at its real 2880x1800 instead of 2560x1600) and MIPQ (Lanczos-2
mip chain instead of the GPU's 2x2 box) make the panel closer to what an ideal display would show?

Method
  1. Input: one lossless 2880x1800 frame of the laptop's real screen (captured with sharp/sharp_probe.py
     --size 2880x1800), converted to linear-light luminance. Text dominates the frame.
  2. The headset cannot show 2560 pixels across the panel: at 25 px/degree a 1.96 m panel seen from ~2 m
     covers ~1100-1300 display pixels. So every path ends at D display pixels across, D = 1000/1200/1400
     (far, typical, close).
  3. Paths, as the headset renders them:
       current      2880 -> 2560 (Sunshine's scaler, modelled as bicubic) -> box mips -> trilinear to D
       MIPQ         same, but the mip chain is built with the Lanczos-2 kernel the headset shader uses
       NATIVE       no 2560 step: 2880 -> box mips -> trilinear to D
       NATIVE+MIPQ  both
  4. Reference: Lanczos-3 resample of the native frame straight to D (the best a D-pixel display can do).
  5. Scores against the reference, in sRGB-encoded luminance: PSNR (dB) and SSIM (9x9 box window).
     Higher is closer to ideal.

Not modelled (they affect every path alike): video compression, and the compositor's own super-sampling,
which filters on top of trilinear. So small differences are meaningful only in sign and rough size.

Result (bench-2880.png, see the commit "Drop NATIVE, SHARPEN, MIPQ, BOOST and AUTO"): MIPQ is worse at
every D; NATIVE is within noise (+0.03..+0.14 dB at D >= 1200, -0.2 dB at 1000) for 25% more bandwidth.

Usage: python3 bench_panel.py FRAME_2880x1800.png   (needs numpy and Pillow)
"""
import sys
import numpy as np
from PIL import Image


# sRGB transfer functions: filtering happens in linear light, scoring in display (sRGB) space.
def to_linear(c): return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
def to_srgb(c): c = np.clip(c, 0, 1); return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


# Float resize through Pillow's 'F' mode (Lanczos, bicubic) for the scaler and the reference.
def resize(img, w, h, method):
    return np.asarray(Image.fromarray(img.astype(np.float32), 'F').resize((w, h), method), dtype=np.float64)


# glGenerateMipmap: each mip texel is the mean of a 2x2 block.
def box_down(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w]; return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


# Lanczos-2 2:1, the same 8 taps per axis the headset shader combines into 4 bilinear fetches.
K = np.array([-0.0089, -0.0419, 0.1165, 0.4343, 0.4343, 0.1165, -0.0419, -0.0089]); K /= K.sum()
def lanczos_down(a):
    def axis(x, ax):
        x = np.moveaxis(x, ax, 0); n = x.shape[0] // 2
        p = np.pad(x, ((3, 3),) + ((0, 0),) * (x.ndim - 1), mode='edge')
        out = sum(K[k] * p[k:k + 2 * n:2] for k in range(8))
        return np.moveaxis(out, 0, ax)
    return np.clip(axis(axis(a, 0), 1), 0, 1)


# Full mip chain down to 1 pixel with the given 2:1 reducer.
def mips(a, down):
    m = [a]
    while min(m[-1].shape) > 1: m.append(down(m[-1]))
    return m


# Bilinear lookup at fractional texel coordinates (clamped to the edge, like GL_CLAMP_TO_EDGE).
def bilinear(a, x, y):
    h, w = a.shape; x = np.clip(x, 0, w - 1); y = np.clip(y, 0, h - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int); x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    return (a[y0, x0] * (1 - fx) + a[y0, x1] * fx) * (1 - fy) + (a[y1, x0] * (1 - fx) + a[y1, x1] * fx) * fy


# GL_LINEAR_MIPMAP_LINEAR at a uniform scale: LOD = log2(source/target), blend the two nearest levels.
def trilinear(chain, W, H):
    s = chain[0].shape[1] / W; lod = np.log2(s); l0 = int(np.floor(lod)); t = lod - l0
    out = []
    for l in (l0, l0 + 1):
        a = chain[l]; sx, sy = a.shape[1] / W, a.shape[0] / H
        X, Y = np.meshgrid((np.arange(W) + 0.5) * sx - 0.5, (np.arange(H) + 0.5) * sy - 0.5)
        out.append(bilinear(a, X, Y))
    return out[0] * (1 - t) + out[1] * t


# SSIM with a (2r+1)^2 box window via integral images; standard constants for a 0..1 range.
def ssim(a, b, r=4):
    def mean(x):
        c = np.cumsum(np.cumsum(np.pad(x, ((1, 0), (1, 0))), 0), 1); k = 2 * r + 1
        return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)
    ma, mb = mean(a), mean(b); va, vb, cov = mean(a * a) - ma ** 2, mean(b * b) - mb ** 2, mean(a * b) - ma * mb
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    return float(np.mean((2 * ma * mb + c1) * (2 * cov + c2) / ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2))))


def psnr(a, b): return float(10 * np.log10(1 / np.mean((a - b) ** 2)))


def main():
    rgb = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.float64) / 255
    lum = to_linear(rgb) @ np.array([0.2126, 0.7152, 0.0722])          # linear luminance, 2880x1800
    H0, W0 = lum.shape
    stream2560 = resize(lum, 2560, 1600, Image.BICUBIC)                 # today: Sunshine scales to 2560x1600
    paths = {'current (2560, box mips)': (stream2560, box_down), 'MIPQ (2560, Lanczos mips)': (stream2560, lanczos_down),
             'NATIVE (2880, box mips)': (lum, box_down), 'NATIVE+MIPQ (2880, Lanczos)': (lum, lanczos_down)}
    chains = {k: mips(a, d) for k, (a, d) in paths.items()}
    for D in (1000, 1200, 1400):
        Hd = round(D * H0 / W0)
        ref = to_srgb(resize(lum, D, Hd, Image.LANCZOS))
        print(f'\nheadset pixels across the panel: {D}  (2560 source shrunk {2560 / D:.2f}x)')
        for k, chain in chains.items():
            out = to_srgb(trilinear(chain, D, Hd))
            print(f'  {k:30s} PSNR {psnr(out, ref):6.2f} dB   SSIM {ssim(out, ref):.4f}')


if __name__ == '__main__':
    main()
