"""Make a first-pass layered PSD from the approved whale-girl master.

This is a reproducible separation draft, not a finished painted rig. It keeps
the source pixels in their original canvas coordinates and marks uncovered
eye/mouth backing as temporary paint for later hand cleanup.
Requires Pillow, numpy and psd-tools in the asset-authoring Python environment.
"""

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from psd_tools import PSDImage


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "art/live2d/whale-girl-neutral-master.png"
OUTPUT = ROOT / "art/live2d/whale-girl-layered-draft.psd"
PREVIEW = ROOT / "build/psd2live/layer-previews"


def polygon(size: tuple[int, int], points: list[tuple[int, int]]) -> np.ndarray:
    canvas = Image.new("L", size, 0)
    ImageDraw.Draw(canvas).polygon(points, fill=255)
    return np.asarray(canvas) > 0


def ellipse(size: tuple[int, int], box: tuple[int, int, int, int]) -> np.ndarray:
    canvas = Image.new("L", size, 0)
    ImageDraw.Draw(canvas).ellipse(box, fill=255)
    return np.asarray(canvas) > 0


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    width, height = source.size
    if (width, height) != (1254, 1254):
        raise ValueError(f"Masks need review for a {width}x{height} master")
    pixels = np.asarray(source).copy()
    opaque = pixels[:, :, 3] > 0
    yy, xx = np.mgrid[0:height, 0:width]

    # Core pixels belong to one layer. Lower layers also retain a small overlap
    # under their neighbours: independently deformed cutouts must not butt up.
    left_eye = ellipse(source.size, (477, 423, 594, 510))
    right_eye = ellipse(source.size, (665, 416, 780, 507))
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

    candidates = [
        ("eyewhite-l", left_eye),
        ("eyewhite-r", right_eye),
        ("mouth", mouth),
        ("front hair", front_hair),
        ("face", face),
        ("handwear-l", left_arm),
        ("handwear-r", right_arm),
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
    PREVIEW.mkdir(parents=True, exist_ok=True)
    for name, candidate in candidates:
        mask = candidate & opaque & ~assigned
        assigned |= mask
        # Original colours in a 12px hidden overlap avoid cutout seams when the
        # neighbouring meshes move. Never expand the exterior silhouette.
        expanded = np.asarray(Image.fromarray(mask.astype(np.uint8) * 255)
                              .filter(ImageFilter.MaxFilter(25))) > 0
        if name not in ("eyewhite-l", "eyewhite-r", "mouth", "handwear-l", "handwear-r"):
            # Never copy moving foreground pixels into a lower layer: that
            # would leave a second eye/hand visible when the real one moves.
            movable = left_eye | right_eye | mouth | left_arm | right_arm
            mask = (expanded & opaque & ~movable) | mask
        layer = pixels.copy()
        layer[:, :, 3] = np.where(mask, pixels[:, :, 3], 0)
        layer[~mask, :3] = 0
        image = Image.fromarray(layer, "RGBA")
        image.save(PREVIEW / f"{len(layers):02d}-{name.replace(' ', '-')}.png")
        layers.append((name, image))

    # The source is flat. Underpaint the areas exposed when automatic eye
    # deformers close; otherwise their cutouts show transparency.
    backing = Image.new("RGBA", source.size, (0, 0, 0, 0))
    backing_pixels = np.asarray(backing).copy()
    for mask in (left_eye, right_eye):
        # Match the bridge-of-nose skin. The earlier saturated pink fill left
        # a conspicuous oval around each eye when the eyelid closed.
        backing_pixels[mask] = (253, 235, 224, 255)
    backing = Image.fromarray(backing_pixels, "RGBA")
    # The rig compresses the painted eyelash into the closed-eye line. Adding
    # another line here creates a second eyelid outline beneath it.
    backing.save(PREVIEW / "feature-backing.png")
    layers.insert(5, ("face detail backing", backing))

    # A closed-eye mesh supplies the lash only as EyeOpen approaches zero.
    # The eye-white mesh alone collapses its dark pixels almost to zero height.
    for name, points in (
        ("eye close-l", [(477, 453), (487, 465), (504, 478), (525, 485),
                         (546, 485), (567, 478), (584, 465), (595, 453)]),
        ("eye close-r", [(665, 447), (677, 460), (692, 474), (713, 481),
                         (735, 481), (754, 473), (771, 459), (780, 447)]),
    ):
        lid = Image.new("RGBA", source.size, (0, 0, 0, 0))
        ImageDraw.Draw(lid).line(points, fill=(57, 38, 64, 255), width=5, joint="curve")
        layers.insert(0, (name, lid))

    # Flat art has no pixels behind the sleeves. Sample adjacent hair into the
    # original sleeve footprints so rotating a sleeve cannot reveal the desktop.
    arm_backing_pixels = np.zeros_like(pixels)
    for mask, offset in ((left_arm, -90), (right_arm, 90)):
        sample_x = np.clip(xx + offset, 0, width - 1)
        sampled = pixels[yy, sample_x]
        color = np.where(sampled[:, :, 3:4] > 0,
                         sampled[:, :, :3], np.array([49, 79, 153]))
        arm_backing_pixels[mask, :3] = color[mask]
        arm_backing_pixels[mask, 3] = pixels[mask, 3]
    arm_backing = Image.fromarray(arm_backing_pixels, "RGBA")
    arm_backing.save(PREVIEW / "arm-backing.png")
    layers.insert(-1, ("arm backing", arm_backing))

    # Vector-authored prop. Its mesh shares the hand's arm/reach keys and has
    # independent sway; it is hidden by a parameter outside the interaction.
    grass = Image.new("RGBA", source.size, (0, 0, 0, 0))
    grass_draw = ImageDraw.Draw(grass)
    stem = [(871, 827), (866, 746), (848, 652), (825, 574)]
    grass_draw.line(stem, fill=(45, 89, 54, 255), width=7, joint="curve")
    grass_draw.line(stem, fill=(118, 161, 78, 255), width=3, joint="curve")
    grass_draw.polygon([(856, 668), (878, 637), (888, 605), (869, 630)],
                       fill=(103, 150, 75, 255))
    for i in range(19):
        y, x = 574 - i * 4, 825 - i * 1.3
        extent = 12 * (1 - abs(i - 9) / 12)
        grass_draw.line([(x - extent, y - 8), (x, y + 4), (x + extent, y - 5)],
                        fill=(150 + i % 3 * 8, 174 + i % 2 * 9, 92, 255), width=4)
    grass.save(PREVIEW / "grass-prop.png")
    # PSD2Live pairs by the base source name after removing a side suffix.
    # This alias puts the prop in the same normalization frame as the hands.
    layers.insert(0, ("handwear right", grass))

    # Save in reverse visual order. An eye/face backing pass will be painted
    # when the automatic rig has been reviewed; the first draft avoids drawing
    # synthetic detail that could compromise the approved character identity.
    # Keep an alpha channel in the document composite. A plain RGB PSD gets
    # a black opaque base that PSD2Live correctly imports as a black rectangle.
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
        if image is not grass and not name.startswith("eye close"):
            composite = Image.alpha_composite(composite, image)
    composite.save(PREVIEW / "composite-check.png")
    composite_pixels = np.asarray(composite).astype(np.int16)
    difference = np.abs(composite_pixels[opaque] - pixels[opaque].astype(np.int16))
    print(f"Saved {OUTPUT} with {len(layers)} layers")
    print(f"Original opaque pixels assigned: {int(assigned.sum())}/{int(opaque.sum())}")
    print(f"Opaque composite max/mean pixel delta: {difference.max()}/{difference.mean():.2f}")


if __name__ == "__main__":
    main()
