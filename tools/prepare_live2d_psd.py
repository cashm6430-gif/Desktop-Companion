"""Make a first-pass layered PSD from the approved whale-girl master.

This is a reproducible separation draft, not a finished painted rig. It keeps
the source pixels in their original canvas coordinates and marks uncovered
eye/mouth backing as temporary paint for later hand cleanup.
Requires Pillow, numpy and psd-tools in the asset-authoring Python environment.
"""

from pathlib import Path
import json

from live2d_paths import LAYERS, enable_authoring_dependencies
enable_authoring_dependencies()

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from psd_tools import PSDImage
from live2d_laptop_rig import seated_layers


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "art/live2d/whale-girl-neutral-master.png"
GRIP = ROOT / "art/live2d/whale-girl-hand-grip-v1.png"
SMILE = ROOT / "art/live2d/whale-girl-mouth-smile-v1.png"
GAPE = ROOT / "art/live2d/whale-girl-mouth-gape-neutral-v1.png"
PLATE = ROOT / "art/live2d/whale-girl-clean-plate-v1.png"
OUTPUT = ROOT / "art/live2d/whale-girl-layered-draft.psd"
PREVIEW = LAYERS


def polygon(size: tuple[int, int], points: list[tuple[int, int]]) -> np.ndarray:
    canvas = Image.new("L", size, 0)
    ImageDraw.Draw(canvas).polygon(points, fill=255)
    return np.asarray(canvas) > 0


def ellipse(size: tuple[int, int], box: tuple[int, int, int, int]) -> np.ndarray:
    canvas = Image.new("L", size, 0)
    ImageDraw.Draw(canvas).ellipse(box, fill=255)
    return np.asarray(canvas) > 0


def skin_underpaint(pixels: np.ndarray, removal: np.ndarray) -> np.ndarray:
    """Reconstruct the native face layer from nearby skin, without hard ovals."""
    r, g, b = [pixels[:, :, i].astype(np.int16) for i in range(3)]
    valid = (r > 215) & (g > 150) & (b > 140) & (r > g + 6) & (g > b + 2) & ~removal
    # Do not divide separately rounded 8-bit blurs: low weights near hair
    # amplify quantization into visible stripes and checkerboard pixels.
    coordinates = np.arange(-54, 55, dtype=np.float32)
    kernel = np.exp(-0.5 * (coordinates / 18) ** 2)
    kernel /= kernel.sum()
    def blur(values):
        values = np.apply_along_axis(lambda row: np.convolve(row, kernel, mode="same"), 0, values)
        return np.apply_along_axis(lambda row: np.convolve(row, kernel, mode="same"), 1, values)
    weight = blur(valid.astype(np.float32))
    colors = blur(np.where(valid[:, :, None], pixels[:, :, :3], 0).astype(np.float32))
    filled = np.clip(colors / np.maximum(weight[:, :, None], 1e-5), 0, 255).astype(np.uint8)
    result = pixels.copy()
    result[removal, :3] = filled[removal]
    return result


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    width, height = source.size
    if (width, height) != (1254, 1254):
        raise ValueError(f"Masks need review for a {width}x{height} master")
    pixels = np.asarray(source).copy()
    plate = Image.open(PLATE).convert("RGBA").resize(source.size, Image.Resampling.LANCZOS)
    plate_pixels = np.asarray(plate).copy()
    opaque = pixels[:, :, 3] > 0
    yy, xx = np.mgrid[0:height, 0:width]

    # Core pixels belong to one layer. Lower layers also retain a small overlap
    # under their neighbours: independently deformed cutouts must not butt up.
    # The old ellipses missed the outer lashes and included moving skin. Trace
    # the entire painted eye instead, then remove the surrounding skin pixels.
    r, g, b = [pixels[:, :, i].astype(np.int16) for i in range(3)]
    skin = (r > 205) & (g > 140) & (b > 120) & (r > g + 6) & (g > b + 2)
    left_eye_region = polygon(source.size, [
        (478, 444), (493, 444), (504, 438), (500, 427), (514, 432),
        (516, 420), (525, 429), (545, 426), (570, 433), (596, 450),
        (601, 455), (594, 483), (581, 498), (561, 506), (521, 510),
        (498, 504), (486, 491), (479, 479), (473, 472), (468, 467),
        (477, 461), (485, 455), (476, 449),
    ])
    right_eye_region = polygon(source.size, [
        (659, 445), (673, 430), (691, 419), (711, 414), (730, 412),
        (737, 405), (738, 415), (758, 419), (772, 424), (782, 419),
        (781, 431), (792, 440), (803, 442), (791, 452), (785, 470),
        (772, 485), (752, 497), (723, 499), (696, 494), (676, 482), (665, 462),
    ])
    # The clean plate supplies the actual background under the outer lashes,
    # including blue hair. Keep only the source features that differ from it;
    # otherwise a skin/blue polygon would move with the blinking eye.
    difference = np.max(np.abs(pixels[:, :, :3].astype(np.int16) - plate_pixels[:, :, :3].astype(np.int16)), axis=2)
    left_eye = left_eye_region & (difference > 22) & ~skin
    right_eye = right_eye_region & (difference > 22) & ~skin
    # A one-pixel antialias band belongs to the eye; no large skin cutout moves
    # with EyeOpen. The face underneath remains solid throughout the blink.
    left_eye = np.asarray(Image.fromarray(left_eye.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(3))) > 0
    right_eye = np.asarray(Image.fromarray(right_eye.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(3))) > 0
    left_iris = ellipse(source.size, (506, 438, 576, 504)) & left_eye & (b > r + 15) & (b > g + 5)
    right_iris = ellipse(source.size, (683, 429, 753, 497)) & right_eye & (b > r + 15) & (b > g + 5)
    # The upper lid keeps its painted thickness while its centreline moves.
    # Eye white is an independent aperture mask; it must not contain lashes.
    left_lash_edge = np.interp(xx, [468, 485, 497, 511, 532, 551, 574, 601],
                             [470, 491, 477, 452, 442, 442, 449, 464])
    right_lash_edge = np.interp(xx, [659, 676, 697, 720, 742, 762, 781, 803],
                              [454, 438, 428, 425, 431, 446, 480, 450])
    # Bright aperture pixels belong to the white, including the thin band at
    # the painted lid boundary. Moving them with the lash leaves a dotted rim.
    left_lash = left_eye & (yy <= left_lash_edge) & ~left_iris & (g < 180)
    right_lash = right_eye & (yy <= right_lash_edge) & ~right_iris & (g < 180)
    mouth = ellipse(source.size, (602, 508, 663, 543))
    # Follow the painted sleeve/cuff/fingers rather than taking a wide slice
    # of the adjacent hair, which would move a blue rectangle with the hand.
    right_arm_outline = [
        (723, 610), (748, 617), (767, 636), (772, 650), (767, 676),
        (786, 700), (822, 733), (876, 774), (882, 784), (875, 797),
        (888, 810), (912, 822), (914, 827), (909, 832), (896, 833),
        (903, 840), (898, 844), (883, 841), (870, 833), (871, 845),
        (868, 855), (861, 852), (846, 832), (831, 818), (829, 834),
        (818, 839), (806, 825), (789, 804), (750, 748), (719, 706),
        (708, 680), (704, 666), (711, 642), (719, 627),
    ]
    right_arm = polygon(source.size, right_arm_outline)
    left_arm = polygon(source.size, [(1254 - x, y) for x, y in right_arm_outline])
    right_arm = np.asarray(Image.fromarray(right_arm.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(9))) > 0
    left_arm = np.asarray(Image.fromarray(left_arm.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(9))) > 0
    # Separate the existing open palm from the sleeve. A native scalar parameter
    # crossfades this small mesh into the authored grip, leaving the sleeve rig
    # continuous. Preserve the cuff, including its white opening rim.
    open_hand = polygon(source.size, [
        (855, 788), (881, 805), (920, 819), (922, 849),
        (879, 860), (854, 859), (832, 837), (825, 812),
    ]) & right_arm
    # Cut through the plain cloth below the puff sleeve. Keep a 24px overlap
    # under the forearm so bending the elbow cannot reveal a cut edge.
    right_lower = right_arm & (xx + yy >= 1467)
    left_lower = left_arm & (1254 - xx + yy >= 1467)
    right_upper = right_arm & (xx + yy <= 1503)
    left_upper = left_arm & (1254 - xx + yy <= 1503)
    tail = polygon(source.size, [
        (970, 580), (1110, 570), (1220, 650), (1249, 822),
        (1180, 953), (1040, 1025), (911, 973), (928, 799),
    ])
    left_leg = polygon(source.size, [
        (489, 932), (628, 932), (630, 1238), (482, 1253),
    ])
    right_leg = polygon(source.size, [
        (621, 932), (763, 932), (767, 1253), (625, 1244),
    ])
    skirt = polygon(source.size, [
        (464, 754), (777, 752), (904, 890), (981, 991),
        (824, 1068), (425, 1068), (278, 996), (348, 896),
    ])
    torso = polygon(source.size, [
        (495, 572), (754, 565), (807, 790), (747, 830),
        (492, 831), (441, 777),
    ])
    head = polygon(source.size, [
        (406, 107), (712, 97), (910, 173), (1019, 416),
        (947, 583), (809, 640), (465, 643), (278, 547),
        (240, 409), (329, 217),
    ])

    # Blue hair in front of the face; the dark eyes are reserved above.
    r, g, b = [pixels[:, :, i].astype(np.int16) for i in range(3)]
    # Keep only the painted smile in the mouth art mesh. Moving a whole skin
    # oval leaves a visible O-shaped seam against the face in native Cubism.
    mouth &= (yy >= 515) & (yy <= 530) & ((r - g) > 38) & (g < 225)
    blue_hair = (b > r + 13) & (b > g + 3) & (b > 48)
    front_hair = head & blue_hair & (yy < 588) & (xx > 286) & (xx < 949)
    face = head & (xx > 423) & (xx < 826) & (yy > 341) & (yy < 599)
    removal = np.asarray(Image.fromarray((left_eye | right_eye | mouth).astype(np.uint8) * 255)
                         .filter(ImageFilter.MaxFilter(9))) > 0
    # The painted plate has uninterrupted skin and a natural hair boundary;
    # generic skin fill on an outer lash would paint a pink ring into the hair.
    face_pixels = skin_underpaint(plate_pixels, ellipse(source.size, (602, 508, 663, 543)))
    pr, pg, pb = [plate_pixels[:, :, i].astype(np.int16) for i in range(3)]

    candidates = [
        # The distal limbs can travel across the face. Keep them in front of
        # every facial drawable, as the gripping hand already is, so a sleeve
        # cannot disappear behind a cheek while its palm stays in front.
        ("handwear_r", open_hand),
        ("handwear-l", left_lower),
        ("handwear-r", right_lower),
        ("eyelash-l", left_lash),
        ("eyelash-r", right_lash),
        ("irides-l", left_iris),
        ("irides-r", right_iris),
        ("eyewhite-l", left_eye),
        ("eyewhite-r", right_eye),
        ("mouth", mouth),
        ("front hair", front_hair),
        ("face", face),
        ("upperarm-l", left_upper),
        ("upperarm-r", right_upper),
        ("tail", tail),
        ("footwear-l", left_leg),
        ("footwear-r", right_leg),
        ("bottomwear", skirt),
        ("topwear", torso),
        ("headwear", head),
        ("back hair", np.ones((height, width), dtype=bool)),
    ]

    assigned = np.zeros((height, width), dtype=bool)
    layers: list[tuple[str, Image.Image]] = []
    previews = {}
    PREVIEW.mkdir(parents=True, exist_ok=True)
    for name, candidate in candidates:
        mask = candidate & opaque & ~assigned
        assigned |= mask
        # Original colours in a 12px hidden overlap avoid cutout seams when the
        # neighbouring meshes move. Never expand the exterior silhouette.
        expanded = np.asarray(Image.fromarray(mask.astype(np.uint8) * 255)
                              .filter(ImageFilter.MaxFilter(25))) > 0
        if name not in ("eyelash-l", "eyelash-r", "irides-l", "irides-r", "eyewhite-l", "eyewhite-r", "mouth", "handwear-l", "handwear-r", "handwear_r", "upperarm-l", "upperarm-r"):
            # Never copy moving foreground pixels into a lower layer: that
            # would leave a second eye/hand visible when the real one moves.
            movable = removal | left_arm | right_arm
            mask = ((expanded & opaque & ~movable) | mask) & ~removal
        layer = pixels.copy()
        if name in ("upperarm-l", "upperarm-r"):
            # Retain the shared cloth under the foreground forearm. Assignment
            # remains unique, but these pixels deliberately exist in both meshes.
            mask = candidate & opaque
        if name == "face":
            # Full painted backing under both the skin and overlapping hair.
            # A skin-only matte leaves holes when outer lashes cross the hair.
            mask |= face
            layer = face_pixels.copy()
        elif name in ("front hair", "back hair", "headwear", "topwear", "bottomwear"):
            # Only static layers use the clean plate. Original moving eyes,
            # hands and sleeves retain their approved source pixels.
            layer = plate_pixels.copy()
            if name == "front hair":
                plate_blue = (pb > pr + 13) & (pb > pg + 3)
                mask = (mask & plate_blue) | (removal & plate_blue)
        elif name in ("eyewhite-l", "eyewhite-r"):
            iris = left_iris if name.endswith("-l") else right_iris
            mask |= iris
            # White stays behind the separately clipped, gaze-driven iris.
            layer[iris, :3] = (250, 248, 252)
        layer[:, :, 3] = np.where(mask, layer[:, :, 3], 0)
        layer[~mask, :3] = 0
        image = Image.fromarray(layer, "RGBA")
        preview_path = PREVIEW / f"{len(layers):02d}-{name.replace(' ', '-')}.png"
        image.save(preview_path)
        previews[name] = preview_path.name
        layers.append((name, image))
    # Layer order can change during occlusion work. Never choose an old preview
    # merely because a glob happens to return an earlier numeric prefix first.
    (PREVIEW / "manifest.json").write_text(json.dumps(previews, indent=2), encoding="utf8")

    # There is no separate ellipse backing: it would expose its border when
    # the eye region and face contour use different deformations.
    # The opening mouth is the mesh that has to carry a bite, so it is drawn at
    # the size a bite needs rather than at a placeholder. The first pass pasted
    # this 1122x760 painted gape at 48x24 -- smaller than the 41x6 resting smile
    # it replaces, and with its 1.48 aspect squashed into 2.0 -- so
    # ParamMouthOpenY moved about ten pixels on the 280 px window and read as
    # nothing. Sizing settled at 120 px across (28% of the 426 px face), but the
    # position took three attempts: bottom-anchoring hung the whole gape below
    # the resting smile line (y 515..530) and read as "mouth on the chin";
    # top-edge anchoring still left the visual mass below the line, because the
    # art's own opening fills most of its content box. The fix the user asked
    # for is geometric and simple: centre the gape's content box on the resting
    # smile's centre, so the opening straddles the line it replaces instead of
    # hanging off it -- upper lip rises above the line, lower lip dips below.
    # 2026-10-01 anchor calibration (render-space measurement, delete-v6 C):
    # the resting omega renders at (423, 357) on the 840 px frame, i.e. source
    # y ~532 -- not 522. The smile art anchored at 522 rendered ~7 px above the
    # omega, which is exactly the "open mouth drifts upward" the user kept
    # rejecting. Both open-mouth layers now anchor at (632, 532) so they land
    # on the omega when rendered. Two open-mouth arts coexist, selected by
    # ParamSmileOpen as a three-state switch (0 closed omega / 1 smile-open /
    # 2 neutral gape): the smile is an emotion (approved grass/busy-stand keep
    # using it untouched), the neutral gape is the mechanical bite for
    # delete-v6. Interpolation between keys is the same linear cross-fade the
    # existing 0.2/0.75 values already rely on.
    SMILE_WIDTH = 120
    # BISECT (build 7): back to 522. The 532 shift -- chosen to align the art
    # with the omega's RENDERED centre -- coincides exactly with the mouth
    # auto-rig corrupting the smile mesh (teeth gone, crescent shape), and the
    # corruption survived every role/name change of the second art. Suspect:
    # the rig derives its open/close mapping from the art position relative to
    # the resting omega line (515..530) and an off-line art folds the mesh.
    SMILE_CENTER = (632, 522)
    smile = Image.new("RGBA", source.size, (0, 0, 0, 0))
    gape = Image.new("RGBA", source.size, (0, 0, 0, 0))

    def paste_mouth_art(target: Image.Image, art_path, width: int) -> None:
        art = Image.open(art_path).convert("RGBA")
        # getbbox() on raw alpha keeps the faint glow halo (rows from y~21),
        # which skews the content box downwards by half its height once
        # cropped. Threshold the alpha so the crop is the visible artwork
        # only, or the centring below lands ~13 px low.
        visible = art.getchannel("A").point(lambda v: 255 if v > 100 else 0)
        art = art.crop(visible.getbbox())
        height = round(width * art.height / art.width)
        art = art.resize((width, height), Image.Resampling.LANCZOS)
        target.alpha_composite(
            art,
            (SMILE_CENTER[0] - width // 2, SMILE_CENTER[1] - height // 2),
        )

    paste_mouth_art(smile, SMILE, SMILE_WIDTH)
    paste_mouth_art(gape, GAPE, SMILE_WIDTH)
    layers.insert(0, ("mouth open", smile))
    # The bite art goes onto an invisible backing (alpha=2) that makes its
    # layer bounds clearly DIFFERENT from the smile layer's. With near-ident
    # bounds, PSD2Live's texture/mesh assignment gives BOTH layers the same
    # atlas region and the smile mesh ends up sampling the gape pixels
    # (teeth gone). Verified by bisect builds: the identical pipeline minus
    # this layer renders the smile byte-identical to the pre-gape reference.
    # The backing is top-aligned with the art so the manual MouthOpenY
    # keyforms (pivot at the backing's top edge) pin the upper lip exactly.
    backing = Image.new("RGBA", (140, 70), (0, 0, 0, 2))
    backing.alpha_composite(gape.crop(gape.getbbox()), (10, 0))
    gape = Image.new("RGBA", source.size, (0, 0, 0, 0))
    gape.alpha_composite(backing, (SMILE_CENTER[0] - 60, SMILE_CENTER[1] - 25))
    layers.insert(0, ("bite mouth", gape))

    # Flat art has no pixels behind the sleeves. Sample adjacent hair into the
    # original sleeve footprints so rotating a sleeve cannot reveal the desktop.
    arm_backing_pixels = plate_pixels.copy()
    backing_mask = np.asarray(Image.fromarray((left_arm | right_arm).astype(np.uint8) * 255)
                              .filter(ImageFilter.MaxFilter(97))) > 0
    arm_backing_pixels[~backing_mask] = 0
    arm_backing = Image.fromarray(arm_backing_pixels, "RGBA")
    arm_backing.save(PREVIEW / "arm-backing.png")
    layers.insert(-1, ("arm backing", arm_backing))

    # Vector-authored prop. Its mesh shares the hand's arm/reach keys and has
    # independent sway; it is hidden by a parameter outside the interaction.
    grass = Image.new("RGBA", source.size, (0, 0, 0, 0))
    grass_draw = ImageDraw.Draw(grass)
    # Extend the stem below the fist. The opaque fingers above this drawable
    # occlude its middle section, so the shaft visibly passes through the palm.
    # Sample two cubic spans rather than a polygonal stick. The lower span
    # passes through the grip; the upper span has a continuous soft bow.
    def cubic_points(a, b, c, d):
        return [tuple(round((1-t)**3*a[j] + 3*(1-t)**2*t*b[j]
                            + 3*(1-t)*t*t*c[j] + t**3*d[j]) for j in (0, 1))
                for t in np.linspace(0, 1, 80)]
    stem = cubic_points((909, 895), (910, 881), (912, 864), (913, 852))
    stem += cubic_points((913, 852), (933, 740), (893, 678), (825, 574))[1:]
    grass_draw.line(stem, fill=(45, 89, 54, 255), width=7, joint="curve")
    grass_draw.line(stem, fill=(118, 161, 78, 255), width=3, joint="curve")
    leaf = cubic_points((883, 669), (861, 650), (878, 622), (887, 606))
    leaf += cubic_points((887, 606), (889, 638), (873, 650), (883, 669))[1:]
    grass_draw.polygon(leaf, fill=(103, 150, 75, 255))
    for i in range(19):
        y, x = 574 - i * 4, 825 - i * 1.3
        extent = 12 * (1 - abs(i - 9) / 12)
        grass_draw.line([(x - extent, y - 8), (x, y + 4), (x + extent, y - 5)],
                        fill=(150 + i % 3 * 8, 174 + i % 2 * 9, 92, 255), width=4)
    grass.save(PREVIEW / "grass-prop.png")
    # PSD2Live pairs by the base source name after removing a side suffix.
    # This alias puts the prop in the same normalization frame as the hands.
    layers.insert(0, ("handwear right", grass))

    # Registration of the generated isolated hand, not a replacement for the
    # character. Retain real alpha and resample once from the original cutout.
    grip = Image.new("RGBA", source.size, (0, 0, 0, 0))
    grip_art = Image.open(GRIP).convert("RGBA")
    grip_art = grip_art.resize((118, 118), Image.Resampling.LANCZOS)
    grip.alpha_composite(grip_art, (821, 779))
    grip_pixels = np.asarray(grip).copy()
    grip_pixels[xx + yy < 1643] = 0  # wrist terminates at the existing cuff
    grip = Image.fromarray(grip_pixels, "RGBA")
    grip.save(PREVIEW / "hand-grip-registered.png")
    layers.insert(0, ("handwear.right", grip))

    # Save in reverse visual order. An eye/face backing pass will be painted
    # when the automatic rig has been reviewed; the first draft avoids drawing
    # synthetic detail that could compromise the approved character identity.
    # Keep an alpha channel in the document composite. A plain RGB PSD gets
    # a black opaque base that PSD2Live correctly imports as a black rectangle.
    layers[0:0] = seated_layers(source.size, PREVIEW, previews)
    (PREVIEW / "manifest.json").write_text(json.dumps(previews, indent=2), encoding="utf8")
    psd = PSDImage.new(mode="RGBA", size=source.size, color=(0, 0, 0, 0), depth=8)
    for name, image in reversed(layers):
        bounds = image.getbbox()
        if not bounds:
            continue
        psd.create_pixel_layer(image.crop(bounds), name=name,
                               top=bounds[1], left=bounds[0])
    psd.save(OUTPUT)
    composite = Image.new("RGBA", source.size, (0, 0, 0, 0))
    for name, image in reversed(layers):
        if not name.startswith("busy ") and image is not grass and image is not grip \
                and image is not smile and image is not gape:
            composite = Image.alpha_composite(composite, image)
    composite.save(PREVIEW / "composite-check.png")
    composite_pixels = np.asarray(composite).astype(np.int16)
    difference = np.abs(composite_pixels[opaque] - pixels[opaque].astype(np.int16))
    print(f"Saved {OUTPUT} with {len(layers)} layers")
    print(f"Original opaque pixels assigned: {int(assigned.sum())}/{int(opaque.sum())}")
    print(f"Opaque composite max/mean pixel delta: {difference.max()}/{difference.mean():.2f}")


if __name__ == "__main__":
    main()
