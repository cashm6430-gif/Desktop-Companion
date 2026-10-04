"""Separate the existing raster materials without changing the approved MOC.

The current flat-art extraction copied the collar into Face/FrontHair and the
chin into ArmBacking/BusyTorso. This produces complementary atlas masks from
measured source polygons. The single photographed neck is a separate flexible
material, with a two-source-pixel jaw overlap. Hidden navy cloth padding is
sampled from the existing collar; it never generates stationary skin.

Run with Pillow and numpy. A repacked atlas or rebuilt MOC requires measuring
the source-to-UV maps again: the pinned input hashes reject stale coordinates.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / "assets/live2d/whale-girl"
MOC = MODEL / "whale-girl-layered-draft.moc3"
ATLAS = MODEL / "whale-girl-layered-draft.4096/texture_00.png"
OUTPUT = MODEL / "material-ownership"
EXPECTED_MOC = "df5e86d822959864ba9f07b1967b792ebb17d9407a83c77b2243575a8027c931"
EXPECTED_ATLAS = "e27d3aac6faa2eb0cbfc8e3275ccc57ec563aeaa685cd53a4a0cd5151ad6b90e"
SIZE = (1254, 1254)

# Verified against Native Core UVs and exact complete RGBA chart comparisons.
# sourceX=atlasX+offsetX; sourceY=atlasY+offsetY, atlasY=4096*(1-v).
CHARTS = {
    "face": {"drawable": "ArtMeshFace", "atlas": [2348, 1196, 2774, 1477], "offset": [-1936, -866]},
    "front hair": {"drawable": "ArtMeshFrontHair", "atlas": [1820, 8, 2506, 522], "offset": [-1545, 78]},
    "arm backing": {"drawable": "ArtMeshBackHair", "atlas": [2959, 8, 3638, 358], "offset": [-2671, 550]},
    "busy torso": {"drawable": "ArtMeshObjects8", "atlas": [2790, 1196, 3153, 1448], "offset": [-2358, -630]},
    "topwear": {"drawable": "ArtMeshTopwear", "atlas": [3520, 1196, 3866, 1375], "offset": [-3070, -609]},
}

# The garment boundary stays inside the two side-hair strands. Its upper edge
# follows the photographed jaw; the colour-derived skin guard below keeps the
# chin, neck triangle and their dark contour on the head surface.
GARMENT_POLYGON = [
    (515, 549), (546, 557), (590, 566), (624, 572),
    (657, 569), (700, 557), (738, 546), (739, 589), (738, 614), (519, 614),
]
# Hidden navy garment support behind the moving jaw. The original photograph
# contains no cloth beneath the chin/neck because the head occluded it. Keep
# this padding inside the centre of the shoulder silhouette, overlapping the
# collar behind the bow. Stopping at Y=595 left a transparent seated seam.
# These pixels live only on the independent body surface, never on the head.
UNDERPAINT_POLYGON = [
    (526, 536), (554, 535), (704, 535), (729, 536),
    (738, 548), (739, 610), (519, 610), (515, 586), (515, 551),
]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def polygon(points: list[tuple[int, int]]) -> np.ndarray:
    result = Image.new("L", SIZE)
    ImageDraw.Draw(result).polygon(points, fill=255)
    return np.asarray(result) > 0


def expand(mask: np.ndarray, pixels: int = 1) -> np.ndarray:
    return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(
        ImageFilter.MaxFilter(pixels * 2 + 1))) > 0


def source_chart(atlas: Image.Image, name: str) -> Image.Image:
    chart = CHARTS[name]
    x0, y0, x1, y1 = chart["atlas"]
    dx, dy = chart["offset"]
    result = Image.new("RGBA", SIZE)
    result.paste(atlas.crop((x0, y0, x1, y1)), (x0 + dx, y0 + dy))
    return result


def put_source_mask(atlas_mask: Image.Image, name: str, source_mask: np.ndarray) -> None:
    chart = CHARTS[name]
    x0, y0, x1, y1 = chart["atlas"]
    dx, dy = chart["offset"]
    image = Image.fromarray(source_mask.astype(np.uint8) * 255)
    atlas_mask.paste(image.crop((x0 + dx, y0 + dy, x1 + dx, y1 + dy)), (x0, y0))


def cloth_underpaint(face: Image.Image, garment: np.ndarray,
                     atlas_size: tuple[int, int]) -> tuple[Image.Image, dict]:
    p = np.asarray(face).astype(np.int16)
    r, g, b, a = (p[:, :, k] for k in range(4))
    yy, xx = np.indices(p.shape[:2])
    # Sample only real navy collar material: no skin, white lace, bright side
    # hair, gold jewellery or dark jaw contour. Prefer the upper fabric rather
    # than the lower bow, whose shading would make a dark stripe below the chin.
    seeds = garment & (a > 200) & (yy <= 586) & (r < 110) & (g < 110) & (g > 40) & (b < 160) & (b > 115) & (b > r + 20)
    # A lone dark antialias pixel beside lace is not a fabric sample column.
    available = np.flatnonzero(seeds.sum(axis=0) >= 3)
    if not len(available):
        raise ValueError("No original navy clothing samples are available for hidden material padding.")
    colors = {}
    for x in available:
        colors[int(x)] = np.median(p[seeds[:, x], x, :3], axis=0).astype(np.uint8)
    support = polygon(UNDERPAINT_POLYGON)
    result = np.zeros((SIZE[1], SIZE[0], 4), dtype=np.uint8)
    column_colors = np.zeros((SIZE[0], 3), dtype=np.uint8)
    for x in range(SIZE[0]):
        seed_x = int(available[np.argmin(np.abs(available - x))])
        column_colors[x] = colors[seed_x]
    # Reduce column noise in the photographed samples, keeping the broad
    # original left/right cloth shading. This changes only hidden padding.
    smooth = np.stack([np.median(column_colors[max(0, x - 3):min(SIZE[0], x + 4)], axis=0)
                       for x in range(SIZE[0])]).astype(np.uint8)
    for x in np.flatnonzero(support.any(axis=0)):
        result[support[:, x], x, :3] = smooth[x]
        result[support[:, x], x, 3] = 255
    if (support & ((yy < 535) | (xx < 515) | (xx > 739))).any():
        raise ValueError("Hidden cloth padding left the measured shoulder/neck support bounds.")
    fill_r, fill_g, fill_b = (result[:, :, k].astype(np.int16) for k in range(3))
    if (support & (fill_r > fill_b)).any():
        raise ValueError("Hidden padding must contain navy cloth, never stationary warm skin.")
    atlas = Image.new("RGBA", atlas_size)
    chart = CHARTS["face"]
    x0, y0, x1, y1 = chart["atlas"]
    dx, dy = chart["offset"]
    atlas.paste(Image.fromarray(result).crop((x0 + dx, y0 + dy, x1 + dx, y1 + dy)), (x0, y0))
    sy, sx = np.where(seeds)
    return atlas, {
        "source": "face", "drawable": "ArtMeshFace", "polygon": UNDERPAINT_POLYGON,
        "source_bbox": [int(np.where(support)[1].min()), int(np.where(support)[0].min()), int(np.where(support)[1].max()) + 1, int(np.where(support)[0].max()) + 1],
        "visible_pixel_count": int(support.sum()),
        "original_navy_sample_pixel_count": int(seeds.sum()),
        "original_navy_sample_bbox": [int(sx.min()), int(sy.min()), int(sx.max()) + 1, int(sy.max()) + 1],
        "method": "Per-column median of original navy collar pixels (at least three samples), nearest sampled column across the neck hole and seven-column noise median. No skin or facial pixels are painted.",
        "purpose": "Hidden cloth behind the moving jaw and neck. Render beneath the main head in both standing and seated postures; transferred standing collar remains separately gated.",
        "warm_skin_pixel_count": 0,
        "pixels_above_source_y535": 0,
        "pixels_outside_central_support": 0,
    }


def neck_skin(image: Image.Image, last_row: int) -> np.ndarray:
    p = np.asarray(image).astype(np.int16)
    r, g, b, a = (p[:, :, k] for k in range(4))
    yy, xx = np.indices(p.shape[:2])
    # Warm face/neck pixels. Navy cloth, blue hair and cool white lace fail the
    # red/blue difference; the gold brooch fails the blue minimum. Never touch
    # the separate exposed chest below the bow (begins around source Y=607).
    region = (xx >= 510) & (xx <= 742) & (yy >= 540) & (yy <= last_row)
    skin = region & (a > 0) & (r > 200) & (g > 140) & (b > 115) & (r > b + 18)
    # The photographed jaw has a 3-4 pixel dark contour. A one-pixel skin
    # dilation leaves a second curved black jaw on the stationary garment.
    # Grow only into dark adjacent contour pixels, preserving cool white lace.
    contour_region = (xx >= 510) & (xx <= 742) & (yy >= 540) & (yy <= last_row + 2)
    dark_contour = (a > 0) & (r < 150) & (g < 145) & (b < 125)
    guard = skin | (expand(skin, 4) & dark_contour & contour_region)
    # Close the photographed near-white antialias pixels between the warm
    # cheek and its dark contour. Colour thresholding alone assigns those
    # isolated pixels to the garment and leaves a stationary dotted jaw.
    # The measured last contour pixel in each column defines the boundary;
    # filling upward cannot eat the lace/chest below it.
    has_guard = guard.any(axis=0)
    last = np.where(guard, yy, -1).max(axis=0)
    return (yy >= 540) & (yy <= last[None, :]) & has_guard[None, :]


def statistics(mask: np.ndarray, source: Image.Image) -> dict:
    active = mask & (np.asarray(source)[:, :, 3] > 0)
    yy, xx = np.where(active)
    return {
        "visible_pixel_count": int(active.sum()),
        "source_bbox": [int(xx.min()), int(yy.min()), int(xx.max()) + 1, int(yy.max()) + 1] if len(xx) else None,
    }


def validate_materials(sources: dict[str, Image.Image], garment: np.ndarray,
                       duplicates: dict[str, np.ndarray]) -> dict:
    p = np.asarray(sources["face"]).astype(np.int16)
    r, g, b, a = (p[:, :, k] for k in range(4))
    yy, xx = np.indices(p.shape[:2])
    neck = (yy >= 540) & (yy <= 589) & (a > 0) & (r > 200) & (g > 140) & (b > 115) & (r > b + 18)
    side_hair = (a > 0) & (r < 150) & (g > 100) & (b > 180) & ((xx < 570) | (xx > 690))
    if (garment & neck).any():
        raise ValueError("A neck/cheek pixel was incorrectly assigned to the body.")
    if (garment & side_hair).any():
        raise ValueError("A bright side-hair pixel was incorrectly assigned to the body.")
    if duplicates["busy torso"][607:].any():
        raise ValueError("The exposed chest below the seated bow must be preserved.")
    if garment[:535].any() or any(mask[:535].any() for mask in duplicates.values()):
        raise ValueError("Face features above the jaw must remain untouched.")
    return {
        "neck_skin_transferred_to_body": 0,
        "bright_side_hair_transferred_to_body": 0,
        "seated_chest_at_or_below_source_y607_erased": 0,
        "face_features_above_source_y535_changed": 0,
    }


def validate_neck(face: Image.Image, neck: np.ndarray, erased: np.ndarray,
                  garment: np.ndarray) -> dict:
    pixels = np.asarray(face).astype(np.int16)
    r, g, b, a = (pixels[:, :, k] for k in range(4))
    yy, _ = np.indices(pixels.shape[:2])
    if neck[:568].any() or erased[:570].any():
        raise ValueError("The neck split must not modify the face above its jaw overlap.")
    if (erased & ~neck).any() or (neck & garment).any():
        raise ValueError("The flexible neck must own all erased skin and no transferred cloth.")
    overlap = neck & ~erased & (a > 0)
    if not overlap.any() or (overlap & ((yy < 568) | (yy > 569))).any():
        raise ValueError("The flexible neck must overlap the moving jaw only at source Y=568..569.")
    warm = (a > 0) & (r > 200) & (g > 140) & (b > 115) & (r > b + 18)
    if not (neck & warm).any():
        raise ValueError("The flexible neck texture must contain the original warm skin.")
    if not (neck[591] & (a[591] > 0)).any():
        raise ValueError("The dark neck-tip contour at source Y=591 must stay on the flexible material.")
    return {
        "source_pixels_above_y568_modified": 0,
        "body_clothing_pixels_copied_to_neck": 0,
        "neck_skin_erased_without_flexible_owner": 0,
        "jaw_overlap_source_y": [568, 570],
        "jaw_overlap_visible_pixel_count": int(overlap.sum()),
        "original_warm_skin_pixel_count": int((neck & warm).sum()),
        "neck_tip_y591_visible_pixel_count": int((neck[591] & (a[591] > 0)).sum()),
    }


def diagnostic(atlas: Image.Image, body_mask: Image.Image, duplicate_mask: Image.Image,
               underpaint: Image.Image, neck_mask: Image.Image, neck_erase_mask: Image.Image,
               output: Path, source_masks: dict[str, np.ndarray]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    original = np.asarray(atlas).copy()
    body = original.copy()
    main = original.copy()
    transferred = np.asarray(body_mask) > 0
    removed = np.asarray(duplicate_mask) > 0
    flexible = np.asarray(neck_mask) > 0
    flexible_erased = np.asarray(neck_erase_mask) > 0
    neck = original.copy()
    neck[:, :, 3][~flexible] = 0
    body[:, :, 3][~transferred] = 0
    main[:, :, 3][transferred | removed | flexible_erased] = 0
    Image.fromarray(main).save(output / "main-atlas.png")
    Image.fromarray(neck).save(output / "neck-atlas.png")
    body_image = Image.alpha_composite(underpaint, Image.fromarray(body))
    body_image.save(output / "body-atlas.png")
    underpaint.save(output / "body-underpaint-atlas.png")
    tiles = []
    for name in CHARTS:
        for caption, texture in (("original", atlas), ("main", Image.fromarray(main)), ("body", body_image)):
            tile = source_chart(texture, name).crop((495, 535, 755, 636)).resize((780, 303), Image.Resampling.NEAREST)
            flat = Image.new("RGBA", tile.size, (88, 92, 98, 255))
            flat.alpha_composite(tile)
            tiles.append((name + " / " + caption, flat.convert("RGB")))
    canvas = Image.new("RGB", (780 * 3, 337 * len(CHARTS)), (36, 40, 47))
    draw = ImageDraw.Draw(canvas)
    for i, (caption, tile) in enumerate(tiles):
        x, y = (i % 3) * 780, (i // 3) * 337
        draw.text((x + 8, y + 8), caption, fill="white")
        canvas.paste(tile, (x, y + 30))
    canvas.save(output / "ownership-layers.png")
    neck_tiles = []
    for caption, texture in (("original", atlas), ("main: jaw overlap", Image.fromarray(main)),
                             ("flexible neck", Image.fromarray(neck))):
        tile = source_chart(texture, "face").crop((535, 560, 715, 607)).resize((900, 235), Image.Resampling.NEAREST)
        flat = Image.new("RGBA", tile.size, (88, 92, 98, 255))
        flat.alpha_composite(tile)
        neck_tiles.append((caption, flat.convert("RGB")))
    neck_canvas = Image.new("RGB", (900, 270 * len(neck_tiles)), (36, 40, 47))
    neck_draw = ImageDraw.Draw(neck_canvas)
    for i, (caption, tile) in enumerate(neck_tiles):
        neck_draw.text((8, i * 270 + 8), caption, fill="white")
        neck_canvas.paste(tile, (0, i * 270 + 30))
    neck_canvas.save(output / "neck-ownership-layers.png")
    for name, mask in source_masks.items():
        Image.fromarray(mask.astype(np.uint8) * 255).save(output / (name.replace(" ", "-") + "-source-mask.png"))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--diagnostic-dir", type=Path)
    args = parser.parse_args()
    if sha256(MOC) != EXPECTED_MOC or sha256(ATLAS) != EXPECTED_ATLAS:
        raise SystemExit("MOC/atlas changed. Re-measure the UV/source maps before regenerating ownership masks.")
    atlas = Image.open(ATLAS).convert("RGBA")
    if atlas.size != (4096, 4096):
        raise SystemExit("Expected the pinned 4096-pixel atlas.")
    sources = {name: source_chart(atlas, name) for name in CHARTS}
    face_guard = neck_skin(sources["face"], 589)
    garment = polygon(GARMENT_POLYGON) & ~face_guard
    yy, _ = np.indices(face_guard.shape)
    # The native face grid cannot bend the narrow neck without also moving
    # its jaw. Keep the entire measured skin guard, including its curved dark
    # outline; a central rectangle would strand the side jaw in the main pass.
    # The upper two rows overlap the main moving jaw at the same head anchor.
    flexible_neck = face_guard & (yy >= 568)
    neck_erased = face_guard & (yy >= 570)
    # Navy collar and bow scraps were also assigned to the front-hair chart.
    # Retain real side-hair strands outside the measured garment silhouette.
    # FrontHair also copied portions of the dark jaw line. Face already owns
    # that contour, so erase the entire garment silhouette on this chart.
    front_duplicate = polygon(GARMENT_POLYGON)
    duplicates = {
        "front hair": front_duplicate,
        # This plate also copied upper collar/bow pixels. They must not follow
        # the hair rig over the new body-owned clothing pass.
        "arm backing": neck_skin(sources["arm backing"], 589) | polygon(GARMENT_POLYGON),
        "busy torso": neck_skin(sources["busy torso"], 582),
        "topwear": face_guard,
    }
    validation = validate_materials(sources, garment, duplicates)
    neck_validation = validate_neck(sources["face"], flexible_neck, neck_erased, garment)
    underpaint, underpaint_details = cloth_underpaint(sources["face"], garment, atlas.size)
    body_mask = Image.new("L", atlas.size)
    duplicate_mask = Image.new("L", atlas.size)
    neck_mask = Image.new("L", atlas.size)
    neck_erase_mask = Image.new("L", atlas.size)
    put_source_mask(body_mask, "face", garment)
    put_source_mask(neck_mask, "face", flexible_neck)
    put_source_mask(neck_erase_mask, "face", neck_erased)
    for name, mask in duplicates.items():
        put_source_mask(duplicate_mask, name, mask)
    OUTPUT.mkdir(exist_ok=True)
    body_path, duplicate_path = OUTPUT / "body-clothing-mask.png", OUTPUT / "duplicate-mask.png"
    body_mask.save(body_path)
    duplicate_mask.save(duplicate_path)
    underpaint_path = OUTPUT / "body-underpaint.png"
    underpaint.save(underpaint_path)
    neck_path, neck_erase_path = OUTPUT / "neck-mask.png", OUTPUT / "neck-erase-mask.png"
    neck_mask.save(neck_path)
    neck_erase_mask.save(neck_erase_path)
    details = {
        "schema_version": 1,
        "moc_sha256": EXPECTED_MOC,
        "atlas_sha256": EXPECTED_ATLAS,
        "body_mask_sha256": sha256(body_path),
        "duplicate_mask_sha256": sha256(duplicate_path),
        "underpaint_sha256": sha256(underpaint_path),
        "neck_mask_sha256": sha256(neck_path),
        "neck_erase_mask_sha256": sha256(neck_erase_path),
        "body_mask": "material-ownership/body-clothing-mask.png",
        "duplicate_mask": "material-ownership/duplicate-mask.png",
        "underpaint": "material-ownership/body-underpaint.png",
        "neck_mask": "material-ownership/neck-mask.png",
        "neck_erase_mask": "material-ownership/neck-erase-mask.png",
        "atlas_size": list(atlas.size),
        "mask_format": "8-bit grayscale, 255 means transfer/erase; no raster pixels are repainted",
        "source_canvas": list(SIZE),
        "source_to_atlas_proof": "Native Core UV bounds and full chart RGBA equality measured 2026-10-02; source=(4096*u+offsetX,4096*(1-v)+offsetY)",
        "source_charts": CHARTS,
        "body_clothing": {
            "source": "face", "drawable": "ArtMeshFace", "garment_polygon": GARMENT_POLYGON,
            "skin_guard": "Warm face/neck source pixels plus adjacent 3-4 pixel dark jaw contour; limited to Y=540..591. No side hair transferred.",
            **statistics(garment, sources["face"]),
        },
        "duplicates": [{"source": name, "drawable": CHARTS[name]["drawable"], **statistics(mask, sources[name])} for name, mask in duplicates.items()],
        "hidden_cloth_support": underpaint_details,
        "flexible_neck": {
            "source": "face", "drawable": "ArtMeshFace",
            "mask_rule": "Complete neck_skin(face,589) contour guard intersected with source Y>=568; no narrow central rectangle.",
            "main_erase_rule": "The same complete guard intersected with source Y>=570.",
            "geometry": "Head anchor starts at Y=570; collar anchor at Y=588. Y=568..569 overlaps the jaw and follows the same head endpoint. The original dark tip to Y=591 is retained.",
            **statistics(flexible_neck, sources["face"]),
            "erased_from_main": statistics(neck_erased, sources["face"]),
            "validation": neck_validation,
        },
        "preserved": "Face above source Y=568 remains unchanged. One original neck skin material connects the moving jaw to the collar, with a two-source-pixel jaw overlap. Chest below the bow at source Y>=607 and original side-hair surfaces are retained.",
        "generator": "tools/prepare_material_ownership.py",
        "validation": validation,
    }
    (OUTPUT / "ownership.json").write_text(json.dumps(details, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.diagnostic_dir:
        diagnostic(atlas, body_mask, duplicate_mask, underpaint, neck_mask, neck_erase_mask,
                   args.diagnostic_dir, {"face-body": garment, "flexible-neck": flexible_neck,
                                         "neck-erased": neck_erased, **duplicates})
    print(json.dumps({"body": details["body_clothing"], "duplicates": details["duplicates"],
                      "flexible_neck": details["flexible_neck"]}, indent=2))


if __name__ == "__main__":
    main()
