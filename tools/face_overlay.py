"""Shared semantics for the face-occlusion figures (T7).

Pure numpy, no `bpy` import: `tools/render_face_masks.py` runs inside Blender and
classifies the pixels, `tools/draw_face_occlusion.py` runs on the host and draws
the legend. Both have to agree on what a colour means, so the palette lives here
rather than being written twice.
"""
from __future__ import annotations

import numpy as np

BACKGROUND, OTHER, FACE_VISIBLE, FACE_HAIR, FACE_NEW = range(5)

PALETTE: dict[int, tuple[int, int, int, int]] = {
    BACKGROUND: (0, 0, 0, 0),
    OTHER: (205, 208, 214, 255),          # body, clothes, hat
    FACE_VISIBLE: (58, 186, 110, 255),    # face, nothing in front of it
    FACE_HAIR: (228, 92, 92, 255),        # face the shipped hair already hides
    FACE_NEW: (206, 74, 214, 255),        # face the fitting layers newly hide
}

LEGEND: list[tuple[int, str]] = [
    (FACE_VISIBLE, "脸：可见"),
    (FACE_HAIR, "脸：被头发遮住（出厂就有的）"),
    (FACE_NEW, "脸：被新增层新遮住"),
    (OTHER, "身体 / 衣物"),
]


def classify(face: np.ndarray, covered_now: np.ndarray, covered_new: np.ndarray,
             foreground: np.ndarray) -> np.ndarray:
    """Map the measurement to a class index per pixel.

    `covered_new` wins over `covered_now`: a pixel that only became hidden
    because of the fitting layers is exactly the thing being measured, and it is
    by construction also covered in the full render.
    """
    cls = np.where(foreground, OTHER, BACKGROUND).astype(np.uint8)
    cls[face & covered_now] = FACE_HAIR
    cls[face & covered_new] = FACE_NEW
    cls[face & ~covered_now] = FACE_VISIBLE
    return cls


def to_rgba(cls: np.ndarray) -> np.ndarray:
    rgba = np.zeros((*cls.shape, 4), dtype=np.uint8)
    for index, colour in PALETTE.items():
        rgba[cls == index] = colour
    return rgba


def face_bbox(cls: np.ndarray, pad: float = 0.18) -> tuple[int, int, int, int]:
    """Bounding box of the face pixels, padded -- the head is a small part of a
    full-body frame, and the figure is unreadable without cropping to it."""
    ys, xs = np.where(np.isin(cls, (FACE_VISIBLE, FACE_HAIR, FACE_NEW)))
    if not len(ys):
        h, w = cls.shape
        return 0, 0, w, h
    y0, y1, x0, x1 = int(ys.min()), int(ys.max()) + 1, int(xs.min()), int(xs.max()) + 1
    dy = int(round((y1 - y0) * pad))
    dx = int(round((x1 - x0) * pad))
    h, w = cls.shape
    return (max(0, x0 - dx), max(0, y0 - dy), min(w, x1 + dx), min(h, y1 + dy))
