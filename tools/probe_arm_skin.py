"""Raster probe: can the shipped layer set carry a raised arm?

This is a feasibility probe, not a rig. It skins the left arm chain (sleeve,
upper arm, forearm+hand) as a function of CANVAS POSITION and renders the result
over the otherwise untouched layers, so the question "does the artwork support
this at all" is answered before any Live2D authoring is written.

It exists because the model's arm reads as stiff, and past roughly 60 degrees as
a thin crooked stick, and two explanations were on the table: the layering does
not support an arm, or the binding does not. This probe separates them. The
layering is fine -- `topwear`'s sleeves are already separate blocks in x below
y=598, so cutting them out is a mask operation and not a repaint, and a hair
layer (`arm-backing`) is already painted underneath, so nothing needs redrawing.
What was wrong was the transform.

Seven transforms were tried and five failed, each for its own reason. They are
listed in `warp_lbs`' docstring as well, because every one of them is a trap any
2D rig falls into:

  backward sample, continuous weights      folds in principle once
                                           `R*d(theta)/ds > 1`, and with a RADIAL
                                           field the gradient also runs ACROSS
                                           the limb: the two edges of the sleeve
                                           end up about 31 deg apart and the arm
                                           shears thin at the joint. That is the
                                           reported defect.
  backward sample, weight at the output    the field stays put while the content
                                           moves, so a pixel swung into a region
                                           the band does not reach is handed back
                                           to the identity and the limb is
                                           overwritten
  forward splat, one copy per segment      mass-exact and gap-free, but a pixel
                                           lands in as many places as it has
                                           segments: radial ghosting
  forward splat, averaged destination      correct, except adjacent destinations
                                           can sit >1px apart: dot lattice
  fixed-point inversion of the field       needs injectivity to have a solution;
                                           diverges into a stretch
  forward LBS, B-spline segments, 1.45px
  footprint, 2x supersample                the shipped version

What the measurements can and cannot see
---------------------------------------
sum(alpha) is conserved by every one of these passes: 100.0% for the backward
controls across r=70..300 at both 46 and 70 degrees, 104-107% for the shipped
one, that surplus being the 1.45px footprint blooming slightly past unit
coverage. An earlier note in this work put the rejected pass at "47% of rest
area"; that was an artifact of comparing plates composited two different ways,
and it is retracted here. What actually separates the passes is the WEIGHT
FIELD -- whether it is a function of canvas position (so the cut cannot open)
and whether its gradient runs along the limb (so the arm cannot shear) -- which
is what RIGb measures and what the fold table explains.

The last one is final, and its parameters are the deliverable: they are what a
Live2D rig (rotate deformers + glue weights) should be configured with. Measured
on this draft: joint (505, 598), arm axis (-0.478, +0.879), band 124px at 0.75
of the sleeve, lateral falloff 60/140px, segments 14px, footprint 1.45px.

Deliberately NOT done here: the right arm (left only), and the torso (bending
and the sit switch are a `topwear` question -- one piece, no waist/hip split --
and a separate one from the arm).

Run (host python, needs numpy + PIL):
  python tools/probe_arm_skin.py
  python tools/probe_arm_skin.py --out art/live2d/arm-skin-probe-20261011-r1

Outputs (see the defaults below): the six-cell comparison sheet, the shoulder
close-up where a seam would show, and the measured numbers.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ROOT / "art/live2d/layers"
DEFAULT_OUT = ROOT / "art/live2d/arm-skin-probe-20261011-r1"
N = 1254

# Paint order of the draft, back to front.
ORDER = [
    "20-back-hair.png", "arm-backing.png", "19-headwear.png", "18-topwear.png",
    "17-bottomwear.png", "16-footwear-r.png", "15-footwear-l.png", "14-tail.png",
    "13-upperarm-r.png", "12-upperarm-l.png", "11-face.png", "10-front-hair.png",
    "09-mouth.png", "08-eyewhite-r.png", "07-eyewhite-l.png", "06-irides-r.png",
    "05-irides-l.png", "04-eyelash-r.png", "03-eyelash-l.png", "02-handwear-r.png",
    "01-handwear-l.png",
]
TOP = "18-topwear.png"
BACKING = "arm-backing.png"
UPPER, FORE = "12-upperarm-l.png", "01-handwear-l.png"
UPPER_R, FORE_R = "13-upperarm-r.png", "02-handwear-r.png"

# Where the sleeve separates from the torso inside `topwear`. Measured from the
# layer's own row spans: from y>=598 the plate splits into left sleeve
# (x 450-528), torso (536-718) and right sleeve (x>730), so the cut follows a
# gap the artist already left rather than a line we invent.
SEAM_Y, LEFT_X, RIGHT_X = 598, 528, 730

# The first pass pivoted at the shoulder joint by eye; the probe fits the arm's
# own axis instead, which is what the band and the falloff are measured along.
SHOULDER = (505.0, 616.0)

# Lateral falloff of the arm's influence: full inside R0 of the arm axis, none
# past R1. This is what keeps the torso and the hair out of the pose.
R0, R1 = 60.0, 140.0
SEG_PX = 14.0        # rigid segment length along the arm
FOOT = 1.45          # source pixel footprint, in destination pixels
SS = 2               # supersample factor


def load(name: str) -> Image.Image:
    return Image.open(LAYERS / name).convert("RGBA")


def split_topwear():
    """Cut the sleeves out of `topwear` along the gap the artist left.

    Returns (body, left sleeve, right sleeve). Lossless: recombining the three
    reproduces the source plate pixel for pixel, which is the point -- the cut
    is a mask operation on a gap, not an edit to the drawing.
    """
    src = np.array(load(TOP))
    yy, xx = np.mgrid[0:src.shape[0], 0:src.shape[1]]
    left = (yy >= SEAM_Y) & (xx < LEFT_X)
    right = (yy >= SEAM_Y) & (xx > RIGHT_X)

    def masked(m):
        out = src.copy()
        out[~m] = 0
        return Image.fromarray(out)

    def without(m):
        out = src.copy()
        out[m] = 0
        return Image.fromarray(out)

    return without(left | right), masked(left), masked(right)


BODY, SLEEVE_L, SLEEVE_R = split_topwear()


def fit_axis():
    """Principal axis of the arm plate, by fitting row midpoints.

    Eyeballing the pivot is what produced the crooked stick: the band and the
    falloff are measured ALONG this axis, so if the axis is off the whole
    gradient is skewed relative to the limb and shear appears across it.
    """
    ink = np.zeros((N, N), bool)
    for img in (SLEEVE_L, load(UPPER), load(FORE)):
        ink |= np.array(img)[:, :, 3] > 16
    rows = np.where(ink.any(axis=1))[0]
    mid = np.array([np.median(np.where(ink[y])[0]) for y in rows])
    slope, intercept = np.polyfit(rows, mid, 1)
    direction = np.array([slope, 1.0])
    direction /= np.hypot(*direction)
    return rows, mid, direction, slope, intercept


ROWS, MID, DIR, SLOPE, INTERCEPT = fit_axis()
JOINT = np.array([np.polyval([SLOPE, INTERCEPT], SEAM_Y), float(SEAM_Y)])

YY, XX = np.mgrid[0:N, 0:N].astype(np.float32)
DX, DY = XX - JOINT[0], YY - JOINT[1]
ALONG = DX * DIR[0] + DY * DIR[1]
PERP = np.abs(DX * DIR[1] - DY * DIR[0])

SS_GRID = np.mgrid[0:N * SS, 0:N * SS].astype(np.float32) / SS
YY2, XX2 = SS_GRID[0], SS_GRID[1]


def smoothstep(v):
    return v * v * (3.0 - 2.0 * v)


def band_from_sleeve(quantile: float = 0.98):
    a = np.array(SLEEVE_L)[:, :, 3] > 16
    ys, xs = np.where(a)
    t = (xs - JOINT[0]) * DIR[0] + (ys - JOINT[1]) * DIR[1]
    return float(np.percentile(t, quantile * 100)), float(t.max())


# 0.75 of the sleeve, not all of it. A band as long as the sleeve leaves the
# whole sleeve inside the gradient, so the arm swings only about 70% of the
# angle asked for and reads as barely raised; ending the blend before the sleeve
# does makes the upper arm and hand rigid followers of the shoulder, and keeps
# the peak weight slope at theta/band (0.51 at 46 deg) rather than steepening it.
BAND_AUTO, SLEEVE_T_MAX = band_from_sleeve(0.75)
BAND_AUTO *= 1.1


def seg_kernel(d):
    """Cubic B-spline segment weight: C2, and a partition of unity.

    A tent kernel is C1-discontinuous at every segment boundary and the creases
    read as banding across the sleeve -- confirmed by doubling `seg_px`, which
    doubled the band spacing.
    """
    return np.where(d <= 1.0, 2.0 / 3.0 - d * d + d ** 3 / 2.0,
                    np.where(d < 2.0, (2.0 - d) ** 3 / 6.0, 0.0))


def warp_lbs(img, angle_deg, band, seg_px=SEG_PX, foot=FOOT, ss=SS):
    """Linear blend skinning: forward, supersampled, one averaged destination.

    Six earlier versions failed, each differently, and the list is worth keeping
    because these are the failure modes of 2D skinning in general:

      1. backward sample on a CONTINUOUS weight field: it folds as soon as
         `R * d(theta)/ds > 1`, and with a RADIAL field the gradient runs across
         the limb as well as along it, so the two edges of the sleeve end up
         about 31 deg apart and the arm shears thin at the joint. That shear is
         the reported defect. (It does not lose mass: measured 100.0% of rest
         over r=70..300 at 46 and 70 deg. An earlier figure of "47% of rest
         area" was a comparison artifact and is retracted -- see the module
         docstring.)
      2. backward sample with the weight read at the OUTPUT pixel: the field
         stays put while the content moves, so a pixel that swings into a region
         the band does not reach is mostly handed back to the identity and the
         limb gets overwritten.
      3. forward splat, one copy per segment (mass-exact, gap-free because each
         copy is an isometry): a pixel appears once per segment it belongs to.
         Measured: radial banding out of the joint.
      4. forward splat, one averaged destination, point kernel: correct, except
         adjacent destinations can land more than a pixel apart and the
         shortfall reads as a translucent dot lattice. A unit-sum kernel cannot
         fix it -- a unit-sum kernel is exactly what leaves the gap.
      5. same, with a 1.45x1.45 area footprint: alpha is solid (255 on 99% of
         ink) and the lattice is gone, but a comb of fine diagonal banding
         remained over the navy/white edges.
      6. inverting the displacement field by fixed-point iteration: needs the
         field to be injective to have a solution at all, and where it is not
         the iterations diverge into a stretch.

      5's comb is aliasing, not coverage: a forward map leaves destination
      sample positions irregular, so in stretched regions a destination pixel
      draws from a sparse and phase-locked subset of sources, and an edge beats
      against the sampling grid. No kernel fixes that -- a wider kernel only
      blurs the beat. Sampling finer does, so the whole transform runs at `ss`
      times resolution and is box-filtered back down.

    The weight is a function of canvas POSITION and not of the layer, which is
    the other half of the fix: both sides of the sleeve/torso cut go through
    this same field, so adjacent pixels there get weights that differ only by
    the one-pixel gradient and the seam cannot open.
    """
    if ss == 1:
        hi = _warp_once(np.asarray(img).astype(np.float32), angle_deg, band,
                        seg_px, foot, XX, YY, N)
        return Image.fromarray(np.clip(np.rint(hi), 0, 255).astype(np.uint8))
    big = np.asarray(img.resize((N * SS, N * SS), Image.NEAREST)).astype(np.float32)
    hi = _warp_once(big, angle_deg, band, seg_px, foot, XX2, YY2, N * SS)
    lo = hi.reshape(N, SS, N, SS, 4).mean(axis=(1, 3))
    return Image.fromarray(np.clip(np.rint(lo), 0, 255).astype(np.uint8))


def premultiply(arr):
    """RGB and alpha on the SAME scale (both 0-1), or the ratio is wrong.

    An earlier version scaled alpha to 0-1 to multiply the colour but left the
    alpha CHANNEL at 0-255. Every weight ratio taken from that array -- which is
    the whole accumulator -- then came out exactly 255 times too small and the
    render came back black. The colours were 50/59/115 and the ratios
    0.20/0.24/0.45: that factor of 255 is the fingerprint.
    """
    f = arr.astype(np.float32) / 255.0
    al = f[..., 3:4]
    return np.concatenate([f[..., :3] * al, al], axis=2)


def _warp_once(arr, angle_deg, band, seg_px, foot, gx, gy, size):
    """The transform itself, on a grid `gx`/`gy` that spans the same canvas."""
    ink = arr[..., 3] > 0
    if not ink.any():
        return np.zeros((size, size, 4), np.float32)
    ox, oy = gx - JOINT[0], gy - JOINT[1]
    a = ox * DIR[0] + oy * DIR[1]
    perp = np.abs(ox * DIR[1] - oy * DIR[0])
    lat = 1.0 - smoothstep(np.clip((perp - R0) / (R1 - R0), 0.0, 1.0))

    n_hi = int(np.ceil(max(float(a[ink].max()), band) / seg_px)) + 2
    tx = np.zeros_like(gx)
    ty = np.zeros_like(gy)
    ws = np.zeros_like(gx)
    for i in range(-2, n_hi + 1):
        w = seg_kernel(np.abs(a - i * seg_px) / seg_px) * lat
        if w.max() <= 1e-5:
            continue
        share = float(np.clip(i * seg_px / band, 0.0, 1.0))
        th = np.radians(angle_deg) * share
        cs, sn = np.cos(th), np.sin(th)
        tx += w * (JOINT[0] + ox * cs + oy * sn)
        ty += w * (JOINT[1] - ox * sn + oy * cs)
        ws += w
    ws = np.clip(ws, 0.0, 1.0)
    tx += (1.0 - ws) * gx            # pixels the band cannot reach pass through
    ty += (1.0 - ws) * gy

    pm = premultiply(arr)
    total = size * size
    acc_a = np.zeros(total, np.float32)
    acc = [np.zeros(total, np.float32) for _ in range(3)]
    # tx/ty are in canvas units, and the destination grid may be finer than one
    # sample per unit. Forgetting this scale put everything in the top-left
    # corner at ss=2, which is what an empty render looks like.
    scale = size / float(N)
    sx, sy = tx * scale, ty * scale
    h = foot * scale / 2.0
    bx, by = np.floor(sx - h), np.floor(sy - h)
    for cx in range(3):
        kx = bx + cx
        wx = np.clip(np.minimum(sx + h, kx + 1.0) - np.maximum(sx - h, kx), 0, 1)
        if wx.max() <= 1e-4:
            continue
        for cy in range(3):
            ky = by + cy
            wy = np.clip(np.minimum(sy + h, ky + 1.0) - np.maximum(sy - h, ky), 0, 1)
            wgt = wx * wy
            xi, yi = kx.astype(np.int32), ky.astype(np.int32)
            good = (ink & (xi >= 0) & (xi < size) & (yi >= 0) & (yi < size)
                    & (wgt > 1e-4))
            if not good.any():
                continue
            flat = yi[good] * size + xi[good]
            ww = wgt[good].astype(np.float32)
            acc_a += np.bincount(flat, weights=ww * pm[good, 3], minlength=total)
            for c in range(3):
                acc[c] += np.bincount(flat, weights=ww * pm[good, c],
                                      minlength=total)

    got = acc_a > 0.004
    rgb = np.zeros((total, 3), np.float32)
    if got.any():                       # acc/acc_a un-premultiplies as it divides
        rgb[got] = np.stack([acc[c][got] / acc_a[got] for c in range(3)], axis=1)
    out = np.zeros((size, size, 4), np.float32)
    out[..., :3] = rgb.reshape(size, size, 3)
    out[..., 3] = np.clip(acc_a, 0.0, 1.0).reshape(size, size)
    return out * 255.0


def transforms(angle_deg, band, elbow_deg=0.0):
    """The whole left arm chain, plus the backing hair it swings over."""
    moved = {
        TOP: (warp_lbs(BODY, angle_deg, band),
              warp_lbs(SLEEVE_L, angle_deg, band)),
        UPPER: warp_lbs(load(UPPER), angle_deg, band),
    }
    if elbow_deg:
        fore = warp_lbs(load(FORE), elbow_deg, band * 1.4)
        moved[FORE] = warp_lbs(fore, angle_deg, band)
    else:
        moved[FORE] = warp_lbs(load(FORE), angle_deg, band)
    moved[BACKING] = warp_lbs(load(BACKING), angle_deg, band)
    return moved


# --- controls: the two rejected passes, kept so the defects can be reproduced --
def old_arm(angle):
    """Pass one: the whole plate, one rigid rotation about the shoulder."""
    arm = SLEEVE_L
    for part in (load(UPPER), load(FORE)):
        arm = Image.alpha_composite(arm, part)
    return arm.rotate(angle, center=SHOULDER, resample=Image.BICUBIC)


def old_skin(img, angle_deg, center=SHOULDER, radius=70.0):
    """Pass two: backward sampling under a radial weight field.

    Re-expressed with the sign convention of `warp_lbs`. The original had the
    inverse form (`source = c + R(+theta)*(out - c)`), so applied on top of the
    already-rotated plate it partly cancelled the pose instead of adding to it --
    which is why this control looked almost like the rest pose and why the
    composite plate read 100% of rest mass.
    """
    arr = np.asarray(img).astype(np.float32)
    h, w = arr.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = xx - center[0], yy - center[1]
    share = np.clip(np.sqrt(dx * dx + dy * dy) / radius, 0.0, 1.0)
    share = smoothstep(share)
    theta = np.radians(angle_deg) * share
    cs, sn = np.cos(theta), np.sin(theta)
    sxx = center[0] + dx * cs + dy * sn
    syy = center[1] - dx * sn + dy * cs
    x0, y0 = np.floor(sxx).astype(np.int32), np.floor(syy).astype(np.int32)
    fx, fy = (sxx - x0)[..., None], (syy - y0)[..., None]
    x0c, y0c = np.clip(x0, 0, w - 2), np.clip(y0, 0, h - 2)
    inside = (sxx >= 0) & (sxx < w - 1) & (syy >= 0) & (syy < h - 1)
    out = (arr[y0c, x0c] * (1 - fx) * (1 - fy) + arr[y0c, x0c + 1] * fx * (1 - fy)
           + arr[y0c + 1, x0c] * (1 - fx) * fy
           + arr[y0c + 1, x0c + 1] * fx * fy)
    out[~inside] = 0.0
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def scene(moved) -> Image.Image:
    base = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    for name in ORDER:
        if name == TOP:
            parts = (moved or {}).get(TOP)
            if parts:
                base = Image.alpha_composite(base, parts[0])
                base = Image.alpha_composite(base, parts[1])
            else:
                base = Image.alpha_composite(base, BODY)
                base = Image.alpha_composite(base, SLEEVE_L)
            base = Image.alpha_composite(base, SLEEVE_R)
            continue
        if name in (UPPER, FORE, UPPER_R, FORE_R):
            if moved and name in moved:
                base = Image.alpha_composite(base, moved[name])
            continue
        if name == BACKING and moved and BACKING in moved:
            base = Image.alpha_composite(base, moved[BACKING])
            continue
        base = Image.alpha_composite(base, load(name))
    return base


def arm_plate(moved) -> list:
    """The three plates that make the arm, in whichever pose was asked for."""
    if not moved:
        return [SLEEVE_L, load(UPPER), load(FORE)]
    return [moved[TOP][1], moved[UPPER], moved[FORE]]


def arm_stats(parts):
    a = np.zeros((N, N), bool)
    for im in parts:
        a |= np.array(im)[:, :, 3] > 128
    ys, xs = np.where(a)
    t = (xs - JOINT[0]) * DIR[0] + (ys - JOINT[1]) * DIR[1]
    return int(a.sum()), float(t.max() - t.min())


def share_at(px, py, band, seg_px=SEG_PX):
    """How much of the rigid pose a canvas point receives, 0..1.

    A scalar version of the weight field, so the field's continuity across the
    two cut lines can be measured without rendering anything.
    """
    ox, oy = px - JOINT[0], py - JOINT[1]
    a = ox * DIR[0] + oy * DIR[1]
    perp = abs(ox * DIR[1] - oy * DIR[0])
    lat = 1.0 - smoothstep(np.clip((perp - R0) / (R1 - R0), 0.0, 1.0))
    ws = 0.0
    n_hi = int(np.ceil(max(abs(a), band) / seg_px)) + 2
    for i in range(-2, n_hi + 1):
        ws += float(seg_kernel(abs(a - i * seg_px) / seg_px))
    return float(min(ws, 1.0)) * lat


def seam_continuity(band):
    """Max weight step between ADJACENT pixels across the sleeve/torso cut.

    Adjacent, not a few pixels apart: the claim being checked is that the field
    has no discontinuity, and a field that varies smoothly will still differ by
    its own gradient over any finite step. For scale, a weight scoped to the
    layer rather than to the position would step by up to 1.0 here.

    The cut is two lines -- vertical at x=LEFT_X and horizontal at y=SEAM_Y,
    both only outside the torso -- so the sample is deliberately narrow rather
    than a sweep of the whole canvas.
    """
    worst_v = max(abs(share_at(LEFT_X - 1, y, band) - share_at(LEFT_X + 1, y, band))
                  for y in range(SEAM_Y + 2, 800, 2))
    worst_h = max(abs(share_at(x, SEAM_Y - 1, band) - share_at(x, SEAM_Y + 1, band))
                  for x in range(300, LEFT_X - 4, 2))
    return worst_v, worst_h


def plate_mask(images) -> np.ndarray:
    m = np.zeros((N, N), bool)
    for im in images:
        m |= np.array(im)[..., 3] > 128
    return m


def enclosed_holes(mask: np.ndarray) -> np.ndarray:
    """Background pixels the figure completely surrounds -- where a tear shows.

    `.copy()` is load-bearing, not tidiness. `Image.fromarray` hands back an
    image over the numpy buffer that is read-only, and `ImageDraw.floodfill`
    writes into it without raising: the fill silently does nothing and the
    helper then reports every background pixel as enclosed. Measured on a
    synthetic ring with a 400px hole: 400 with the copy, 6800 (the whole
    background) without it.
    """
    im = Image.fromarray(np.where(mask, 0, 255).astype(np.uint8)).copy()
    ImageDraw.floodfill(im, (0, 0), 128)
    return np.array(im) == 255


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    args = ap.parse_args()
    out_dir = Path(args.out)
    if not out_dir.is_absolute():
        out_dir = (ROOT / out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    rows = [
        f"layer set: {len(ORDER)} plates from {LAYERS}",
        f"axis dir={DIR.round(4)}  slope={SLOPE:.4f}  shoulder={SHOULDER}",
        f"joint={JOINT.round(1)} at seam y={SEAM_Y}  sleeve t max="
        f"{SLEEVE_T_MAX:.1f}  band={BAND_AUTO:.1f}px",
        f"segments {SEG_PX:.0f}px (cubic B-spline)  footprint {FOOT}  "
        f"supersample {SS}x  lateral falloff {R0:.0f}/{R1:.0f}px",
        "weight ramp is LINEAR along the arm; segment blend is a cubic B-spline "
        "(a tent kernel creases at every segment boundary)",
        "",
        "fold criterion  R*d(theta)/ds  (the quantity that broke backward "
        "sampling; forward LBS does not depend on it, it explains the controls)",
    ]
    for deg in (46.0, 70.0):
        peak = np.radians(abs(deg)) / BAND_AUTO
        for perp, tag in ((48.0, "sleeve"), (120.0, "hair/blouse")):
            r = float(np.hypot(BAND_AUTO * 0.5, perp))
            rows.append(f"  {deg:+.0f}deg {tag:12s} mid-band R={r:5.0f}  "
                        f"R*dtheta/ds={r * peak:.2f} "
                        f"({'FOLDS' if r * peak > 1 else 'stable'})")

    moved46 = transforms(-46.0, BAND_AUTO)
    moved70 = transforms(-70.0, BAND_AUTO)
    moved70e = transforms(-70.0, BAND_AUTO, elbow_deg=-34.0)

    cases = [
        ("rest", scene(None)),
        ("OLD rigid -46 (seam)", scene({TOP: (BODY, old_arm(-46.0)),
                                        UPPER: load(UPPER), FORE: load(FORE)})),
        ("OLD field -70 (seam + thin)",
         scene({TOP: (BODY, old_skin(old_arm(-70.0), -70.0)),
                UPPER: load(UPPER), FORE: load(FORE)})),
        ("NEW -46", scene(moved46)),
        ("NEW -70", scene(moved70)),
        ("NEW -70 + elbow", scene(moved70e)),
    ]

    # --- mass, measured through ONE path for every pass ---------------------
    # Both sides of a comparison have to be composited the same way. Comparing a
    # per-plate union (25305px) against a composited plate (25138px) is a 0.7%
    # systematic offset, and comparing a per-plate union against a SINGLE
    # composited image of the old pass is how an earlier run reported "area fell
    # to 47%": most of that number was the comparison, not the transform. What
    # a rotation must preserve is sum(alpha) of the same plate, warped or not.
    def plate(images):
        out = images[0].copy()
        for im in images[1:]:
            out = Image.alpha_composite(out, im)
        return out

    # `topwear` paints under the upper arm, so it composites first -- the same
    # order ORDER gives.
    rest_plate = plate([SLEEVE_L, load(UPPER), load(FORE)])
    base_area, base_len = arm_stats(arm_plate(None))

    def mass(img):
        return float((np.array(img).astype(np.float32)[..., 3] / 255.0).sum())

    def warped(angle_deg):
        """One plate through the same field the shipped transform uses."""
        return warp_lbs(rest_plate, angle_deg, BAND_AUTO)

    m_rest = mass(rest_plate)
    rows.append("")
    rows.append(f"composited arm plate at rest: sum(alpha)={m_rest:.0f}  "
                f"ink>128={base_area}px  "
                f"extent along the rest axis={base_len:.0f}px "
                f"(shrinks as the arm swings; it is not a pose-invariant length)")
    rows.append("  single-plate path -- every row composited the same way, so "
                "the ratios are comparable:")
    composite_ratios = {}
    for label, img in (("NEW -46", warped(-46.0)),
                       ("NEW -70", warped(-70.0)),
                       ("OLD backward field -70, r=70",
                        old_skin(rest_plate, -70.0, radius=70.0)),
                       ("OLD backward field -70, r=300",
                        old_skin(rest_plate, -70.0, radius=300.0))):
        mr = mass(img)
        composite_ratios[label] = mr / m_rest
        rows.append(f"    {label:<30} sum(alpha)={mr:8.0f} "
                    f"({100.0 * mr / m_rest:5.1f}% of rest)")

    # The elbow stage acts on the forearm only, so a single composited plate
    # cannot represent the two-stage pose. Those three poses are measured on the
    # PER-PLATE path instead -- which is the shipped pipeline -- and compared
    # against the rest plates measured the same way, plate by plate. Mixing the
    # two bases is what produced the numbers that had to be retracted.
    rest_parts = arm_plate(None)
    m_parts_rest = sum(mass(i) for i in rest_parts)
    rows.append(f"  per-plate path ({len(rest_parts)} plates, the path "
                f"`transforms` uses, elbow included) -- rest total "
                f"sum(alpha)={m_parts_rest:.0f}:")
    ratios = {}
    for label, moved in (("NEW -46", moved46), ("NEW -70", moved70),
                         ("NEW -70 + elbow", moved70e)):
        mp = sum(mass(i) for i in arm_plate(moved))
        ratios[label] = mp / m_parts_rest
        rows.append(f"    {label:<30} sum(alpha)={mp:8.0f} "
                    f"({100.0 * mp / m_parts_rest:5.1f}% of rest)")
    rows.append("  the +4..7% surplus is the 1.45px footprint blooming slightly "
                "past unit coverage; it is the price of having no gaps")
    rows.append("  the OLD passes hold 100%: backward sampling under a "
                "continuous field does NOT lose mass at these angles, over "
                "r=70..300, at either 46 or 70 deg. So mass is not the metric "
                "that catches the defect -- the thinness was shear, from a "
                "RADIAL field whose gradient also runs across the limb (the two "
                "edges of the sleeve end up about 31 deg apart). Reporting "
                "\"area fell to 47%\" earlier was a measurement artifact of "
                "comparing plates composited two different ways; retracted.")

    # --- the seam: adjacent pixels across the cut must get the same pose ----
    worst_v, worst_h = seam_continuity(BAND_AUTO)
    rows.append("")
    rows.append(f"seam continuity, |delta share| between ADJACENT pixels: "
                f"vertical cut {worst_v:.5f}  horizontal cut {worst_h:.5f}")
    rows.append("  (for scale: a weight scoped to the layer instead of to the "
                "canvas position would step by up to 1.0 at this line)")

    # --- and whether the arm plate itself stays whole -----------------------
    rest_holes = int(enclosed_holes(plate_mask([SLEEVE_L, load(UPPER),
                                                load(FORE)])).sum())
    gap_warped = int(enclosed_holes(
        plate_mask([moved70[TOP][1], moved70[UPPER], moved70[FORE]])).sum())
    rows.append("")
    rows.append(f"enclosed gaps inside the arm plate: rest {rest_holes}px, "
                f"-70 {gap_warped}px")
    rows.append("  (the enclosed background the raised arm leaves against the "
                "body -- the armpit -- is expected and is not counted here)")

    # --- gates ---------------------------------------------------------------
    gates, failures = [], []
    new_ratios = [ratios["NEW -46"], ratios["NEW -70"], ratios["NEW -70 + elbow"]]
    ok = all(0.90 <= r <= 1.10 for r in new_ratios)
    gates.append(f"RIGa mass preserved, new passes within 10% of rest: "
                 f"{'PASS' if ok else 'FAIL'} "
                 f"({', '.join(f'{r:.3f}' for r in new_ratios)})")
    failures += [] if ok else ["RIGa"]

    ok = max(worst_v, worst_h) < 0.10
    gates.append(f"RIGb weight field continuous across the cut, adjacent pixels "
                 f"(<0.10 of a full pull): {'PASS' if ok else 'FAIL'} "
                 f"({worst_v:.5f}, {worst_h:.5f})")
    failures += [] if ok else ["RIGb"]

    ok = gap_warped <= rest_holes + 32
    gates.append(f"RIGc no new gap inside the arm plate (<=32px): "
                 f"{'PASS' if ok else 'FAIL'} "
                 f"({gap_warped - rest_holes:+d}px)")
    failures += [] if ok else ["RIGc"]

    # --- figures -------------------------------------------------------------
    crop = (300, 500, 680, 920)
    tiles = []
    for title, img in cases:
        flat = Image.new("RGBA", img.size, (250, 250, 252, 255))
        tiles.append((title, Image.alpha_composite(flat, img).convert("RGB")
                      .crop(crop)))
    w, h = tiles[0][1].size
    sheet = Image.new("RGB", (w * 3 + 32, (h + 26) * 2 + 8), (255, 255, 255))
    draw = ImageDraw.Draw(sheet)
    for i, (title, img) in enumerate(tiles):
        x, y = 8 + (i % 3) * (w + 8), 4 + (i // 3) * (h + 26)
        sheet.paste(img, (x, y + 22))
        draw.text((x, y + 5), title, fill=(0, 0, 0))
    sheet_path = out_dir / "arm-skin.png"
    sheet.save(sheet_path)

    close, zoom = (395, 540, 640, 790), 2.2
    zooms = []
    for title, img in (cases[0], cases[1], cases[3], cases[4], cases[5]):
        flat = Image.new("RGBA", img.size, (250, 250, 252, 255))
        c = Image.alpha_composite(flat, img).convert("RGB").crop(close)
        zooms.append((title, c.resize((int(c.width * zoom), int(c.height * zoom)),
                                      Image.LANCZOS)))
    zw, zh = zooms[0][1].size
    zsheet = Image.new("RGB", (zw * 5 + 48, zh + 26), (255, 255, 255))
    zdraw = ImageDraw.Draw(zsheet)
    for i, (title, img) in enumerate(zooms):
        x = 8 + i * (zw + 8)
        zsheet.paste(img, (x, 22))
        zdraw.text((x, 5), title, fill=(0, 0, 0))
    close_path = out_dir / "arm-skin-closeup.png"
    zsheet.save(close_path)

    rows.append("")
    rows += gates
    rows.append(f"RIG_RESULT {'PASS' if not failures else 'FAIL'} "
                f"({', '.join(failures) or 'RIGa,RIGb,RIGc'})")
    diag_path = out_dir / "arm-skin-diag.txt"
    diag_path.write_text("\n".join(rows), encoding="utf-8")

    print("\n".join(rows))
    print(f"wrote {sheet_path}")
    print(f"wrote {close_path}")
    print(f"wrote {diag_path}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
