#!/usr/bin/env python3
"""Audit every Live2D drawable's atlas tile for contamination.

Reads the public-graph JSON emitted by tools/PSD2LiveCmoImportProbe.java (or
any public PuppetModel dump) plus the exported texture page(s), crops each
drawable's TRUE tile (its UV bounding box, no padding), writes one PNG per
drawable and a contact sheet, and reports per-tile statistics that flag likely
contamination (foreign art bleeding into a layer's own tile).

Usage:
    python3 tools/audit_live2d_tiles.py --graph <graph.json> \
        --textures <dir with texture_00.png ...> --out <dir>

Nothing is modified; this is read-only analysis. Runs with the uv cpython-3.12
interpreter (or the sandbox python3 that has Pillow/numpy).
"""
import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def uv_box(drawable):
    uv = drawable["getMesh"]["getUvs"]
    xs = uv[0::2]
    ys = uv[1::2]
    return min(xs), min(ys), max(xs), max(ys)


def load_pages(texture_dir):
    pages = []
    i = 0
    while True:
        p = os.path.join(texture_dir, "texture_%02d.png" % i)
        if not os.path.exists(p):
            break
        pages.append(Image.open(p).convert("RGBA"))
        i += 1
    if not pages:
        raise SystemExit("no texture_NN.png found in " + texture_dir)
    return pages


def stats(tile):
    a = np.asarray(tile)
    al = a[:, :, 3]
    opaque = al > 32
    r, g, b = (a[:, :, k].astype(int) for k in range(3))
    # "hair-ish" light blue: blue dominant and fairly light -- a proxy for the
    # blue strands that leak into sleeve/hand/clothing tiles on this model.
    hairish = opaque & (b > 110) & (b > r + 12) & (b >= g - 10)
    return dict(
        px=int(al.size),
        opaque_frac=round(float(opaque.mean()), 4),
        hairblue_frac=round(float(hairish.mean()), 4),
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graph", required=True)
    ap.add_argument("--textures", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--page", type=int, default=0)
    ap.add_argument("--cell", type=int, default=150)
    args = ap.parse_args()

    g = json.load(open(args.graph))
    drawables = g["getDrawables"]
    pages = load_pages(args.textures)
    os.makedirs(args.out, exist_ok=True)
    tiles_dir = os.path.join(args.out, "tiles")
    os.makedirs(tiles_dir, exist_ok=True)

    try:
        from PIL import ImageFont
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
    except Exception:
        font = ImageFont.load_default()

    report = []
    cells = []
    for i, d in enumerate(drawables):
        page = pages[args.page]
        W, H = page.size
        x0, y0, x1, y1 = uv_box(d)
        box = (max(0, int(x0 * W)), max(0, int(y0 * H)), min(W, int(x1 * W)), min(H, int(y1 * H)))
        tile = page.crop(box)
        tile.save(os.path.join(tiles_dir, "%02d.png" % i))
        st = stats(tile)
        st.update(index=i, name=d.get("getName"), uv_box=[round(v, 4) for v in (x0, y0, x1, y1)],
                  tile_px=[tile.width, tile.height], vertices=d["getMesh"]["getVertexCount"])
        report.append(st)
        # cell thumbnail
        scale = min(args.cell / max(1, tile.width), args.cell / max(1, tile.height), 3.0)
        thumb = tile.resize((max(1, int(tile.width * scale)), max(1, int(tile.height * scale))), Image.NEAREST)
        cells.append((i, d.get("getName"), thumb))

    cols = 7
    cw = args.cell + 8
    ch = args.cell + 34
    rows = (len(cells) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cw + 8, rows * ch + 8), (250, 250, 250))
    dr = ImageDraw.Draw(sheet)
    for k, (i, name, thumb) in enumerate(cells):
        r, c = divmod(k, cols)
        x = 8 + c * cw
        y = 8 + r * ch
        sheet.paste(thumb, (x + (args.cell - thumb.width) // 2, y + 18))
        st = report[k]
        dr.text((x, y), "#%d %s" % (i, str(name)[:15]), fill=(0, 0, 0), font=font)
        dr.text((x, y + ch - 15), "op%.2f hb%.2f" % (st["opaque_frac"], st["hairblue_frac"]), fill=(120, 120, 120), font=font)
    sheet.save(os.path.join(args.out, "all_tiles_sheet.png"))
    json.dump(report, open(os.path.join(args.out, "tile_audit.json"), "w"), indent=1)

    # console summary, most "hair-blue" first
    print("drawables:", len(drawables), "-> tiles in", tiles_dir)
    for st in sorted(report, key=lambda s: -s["hairblue_frac"])[:15]:
        print("  #%-2d %-22s op=%.2f hairblue=%.2f tile=%dx%d" % (
            st["index"], str(st["name"])[:22], st["opaque_frac"], st["hairblue_frac"],
            st["tile_px"][0], st["tile_px"][1]))
    print("sheet:", os.path.join(args.out, "all_tiles_sheet.png"))


if __name__ == "__main__":
    sys.exit(main())
