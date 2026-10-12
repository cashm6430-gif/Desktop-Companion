#!/usr/bin/env python3
"""Cut each Live2D drawable's atlas tile to its own MESH footprint.

For every drawable, rasterize its mesh triangles in UV space (the region the
mesh actually samples) and zero the alpha of texels OUTSIDE that region (with a
small outward margin to keep the anti-aliased edge). This removes the foreign
pixels that were packed into a layer's tile but are not covered by that layer's
own mesh -- "cutting to the skin/mesh".

Read-only against the source pages; writes new pages to --out.

Usage:
    python3 tools/cut_live2d_tiles_by_mesh.py --graph <graph.json> \
        --textures <dir> --out <dir> [--margin 3] [--page 0]
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graph", required=True)
    ap.add_argument("--textures", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--margin", type=int, default=3)
    ap.add_argument("--page", type=int, default=0)
    args = ap.parse_args()

    g = json.load(open(args.graph))
    drawables = g["getDrawables"]
    os.makedirs(args.out, exist_ok=True)
    page = Image.open(os.path.join(args.textures, "texture_%02d.png" % args.page)).convert("RGBA")
    arr = np.asarray(page).copy()
    W, H = page.size
    before = after = 0
    for i, d in enumerate(drawables):
        mesh = d["getMesh"]
        uv = mesh["getUvs"]
        idx = mesh["getIndices"]
        if len(idx) < 3:
            continue
        xs = uv[0::2]
        ys = uv[1::2]
        # skip degenerate full-page quads (workstation overlays): their mesh spans
        # the whole page, and cutting them to the page would only risk side effects
        if (max(xs) - min(xs)) > 0.5 or (max(ys) - min(ys)) > 0.5:
            continue
        m = Image.new("L", (W, H), 0)
        dm = ImageDraw.Draw(m)
        for t in range(0, len(idx) - 2, 3):
            poly = []
            for k in (idx[t], idx[t + 1], idx[t + 2]):
                poly.append((uv[2 * k] * W, uv[2 * k + 1] * H))
            dm.polygon(poly, fill=255)
        if args.margin > 0:
            m = m.filter(ImageFilter.MaxFilter(2 * args.margin + 1))
        keep = np.asarray(m) > 0
        # restrict to this drawable's own bbox (+margin) so we never touch far pixels
        xs = uv[0::2]
        ys = uv[1::2]
        x0 = max(0, int(min(xs) * W) - args.margin - 1)
        y0 = max(0, int(min(ys) * H) - args.margin - 1)
        x1 = min(W, int(max(xs) * W) + args.margin + 2)
        y1 = min(H, int(max(ys) * H) + args.margin + 2)
        sub = arr[y0:y1, x0:x1]
        ksub = keep[y0:y1, x0:x1]
        before += int((sub[:, :, 3] > 32).sum())
        sub[:, :, 3] = np.where(ksub, sub[:, :, 3], 0)
        after += int((sub[:, :, 3] > 32).sum())
    Image.fromarray(arr).save(os.path.join(args.out, "texture_%02d.png" % args.page))
    print("cut page %d -> %s  opaque %d -> %d (removed %d)" % (
        args.page, os.path.join(args.out, "texture_%02d.png" % args.page), before, after, before - after))


if __name__ == "__main__":
    sys.exit(main())
