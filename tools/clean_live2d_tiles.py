#!/usr/bin/env python3
"""Clean Live2D atlas tiles: strip the junk that grew around each component.

For every drawable's tile (UV bbox) on the given texture pages, this keeps the
component's main body and erases the stray content that surrounds it (thin
fringes, hair strands, frill overlaps, small attached islands) via a
morphological open (erode -> keep largest blob -> dilate back). The pixel's own
color is untouched; only alpha outside the kept body is zeroed.

Read-only against the source pages: it writes new pages to --out.

Usage:
    python3 tools/clean_live2d_tiles.py --graph <graph.json> \
        --textures <dir with texture_NN.png> --out <dir> [--erode 3] [--page 0]
"""
import argparse
import json
import os
import sys
from collections import deque

import numpy as np
from PIL import Image, ImageFilter


def uv_box(d):
    uv = d["getMesh"]["getUvs"]
    return min(uv[0::2]), min(uv[1::2]), max(uv[0::2]), max(uv[1::2])


def largest_blob(mask):
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    best = None
    best_n = 0
    for yy in range(h):
        row = mask[yy]
        for xx in range(w):
            if row[xx] and not seen[yy, xx]:
                q = deque([(yy, xx)])
                seen[yy, xx] = True
                pts = []
                while q:
                    cy, cx = q.popleft()
                    pts.append((cy, cx))
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            ny, nx = cy + dy, cx + dx
                            if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                                seen[ny, nx] = True
                                q.append((ny, nx))
                if len(pts) > best_n:
                    best_n = len(pts)
                    best = pts
    out = np.zeros_like(mask, dtype=bool)
    if best:
        for cy, cx in best:
            out[cy, cx] = True
    return out


def clean_tile(tile, erode):
    a = np.asarray(tile).copy()
    alpha = Image.fromarray(a[:, :, 3])
    k = 2 * erode + 1
    eroded = alpha.filter(ImageFilter.MinFilter(k))          # shave thin fringes
    core = largest_blob(np.asarray(eroded) > 32)             # keep the body
    grown = Image.fromarray((core * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(k))
    keep = (np.asarray(grown) > 32)
    a[:, :, 3] = np.where(keep, a[:, :, 3], 0)               # erase outside the body
    return Image.fromarray(a), int((np.asarray(tile)[:, :, 3] > 32).sum()), int(keep.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graph", required=True)
    ap.add_argument("--textures", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--erode", type=int, default=3)
    ap.add_argument("--page", type=int, default=0)
    args = ap.parse_args()

    g = json.load(open(args.graph))
    drawables = g["getDrawables"]
    os.makedirs(args.out, exist_ok=True)
    src = os.path.join(args.textures, "texture_%02d.png" % args.page)
    page = Image.open(src).convert("RGBA")
    arr = np.asarray(page).copy()
    W, H = page.size
    before = after = 0
    for i, d in enumerate(drawables):
        x0, y0, x1, y1 = uv_box(d)
        # skip degenerate drawables: full-page quads (workstation overlays) and
        # tiny-vertex meshes have no meaningful per-component tile to clean.
        if d["getMesh"]["getVertexCount"] <= 8 or (x1 - x0) > 0.5 or (y1 - y0) > 0.5:
            continue
        # skip functional BACKING layers: their content deliberately fills the
        # gaps behind other parts, so cleaning them opens holes.
        name = (d.get("getName") or "").lower()
        if "backing" in name:
            continue
        b = (max(0, int(x0 * W)), max(0, int(y0 * H)), min(W, int(x1 * W)), min(H, int(y1 * H)))
        if b[2] - b[0] < 4 or b[3] - b[1] < 4:
            continue
        tile = page.crop(b)
        cleaned, nb, na = clean_tile(tile, args.erode)
        before += nb
        after += na
        arr[b[1]:b[3], b[0]:b[2]] = np.asarray(cleaned)
    out = os.path.join(args.out, "texture_%02d.png" % args.page)
    Image.fromarray(arr).save(out)
    print("cleaned page %d -> %s" % (args.page, out))
    print("opaque px %d -> %d (removed %d)" % (before, after, before - after))


if __name__ == "__main__":
    sys.exit(main())
