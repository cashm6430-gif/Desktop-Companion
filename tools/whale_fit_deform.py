"""T3: component-aware parametric deformation for silhouette fitting.

The r2/r3 authoring blend (art/3d/whale-girl-head-r2/character-round3.blend)
holds ~75 *named* part meshes (HairBack0..4, HairSideOuterL/R, Bang0..10,
Skirt, Sock/Shoe.L/R, TailFlukeA/B, Head, ...). Rather than rebuilding the
character for every candidate, silhouette fitting deforms those parts in place.

Design rules
------------
* Every parameter has a neutral default. When a parameter is neutral its rule
  is *skipped entirely*, so the identity parameter set reproduces the source
  mesh bit-for-bit (gate V3a). This is why the code tests `!= 0.0` instead of
  computing `x + 0.0 * t`, which would not be exactly `x` in floating point.
* All deformation happens in WORLD space and is written back through
  `matrix_world.inverted()`, because roughly a third of the parts carry a
  non-identity object transform (the ellipsoid primitives keep a rotation).
* Each part gets a *ramp* t in [0, 1] along its own extent (0 at the root,
  1 at the tip), smoothed with smoothstep. Hair therefore stays attached to
  the scalp at the root and only flares / lengthens toward the tips - the
  anatomical constraint the art requires.

Everything here is pure math on vertex coordinates: no topology is created or
destroyed, so UVs, vertex groups and material slots are untouched by design.
"""
from __future__ import annotations

import math

try:                                        # importable outside Blender for spec use
    import bpy
    from mathutils import Vector
except Exception:                           # pragma: no cover
    bpy = None
    Vector = None

# name -> (default, low, high, description). Defaults are all neutral.
PARAM_SPEC: dict[str, tuple[float, float, float, str]] = {
    "hair_side_flare":  (0.0, 0.00, 0.22, "lateral push of the outer/side lock tips (model units)"),
    "hair_length":      (0.0, 0.00, 0.45, "extra downward drop of every hair lock tip"),
    "hair_back_volume": (0.0, 0.00, 0.28, "extra +Y (backwards) volume of the back locks"),
    "bang_length":      (0.0, 0.00, 0.22, "extra downward drop of the fringe tips"),
    "ahoge_scale":      (1.0, 0.40, 2.20, "uniform scale of the crown curl about its root"),
    "crown_taper":      (0.0, 0.00, 0.30, "lateral narrowing of the head above z=1.92"),
    "skirt_flare":      (0.0, 0.00, 0.32, "radial push of the skirt, growing toward the hem"),
    "skirt_drop":       (0.0, 0.00, 0.22, "extra downward drop of the skirt hem"),
    "foot_length":      (1.0, 0.45, 2.00, "Y scale of the shoes about their own centre"),
    "foot_height":      (1.0, 0.45, 3.00, "Z scale of the shoes about the SOLE (grows upward only)"),
    "tail_flare":       (1.0, 0.50, 2.50, "uniform scale of the tail flukes about their centre"),
    # --- part creation -------------------------------------------------------
    # Deforming the existing parts has a hard ceiling: the reference contains
    # components the model does not have at all (long locks falling in front of
    # the shoulders, extra outer volume, a deeper back mass). These parameters
    # *instantiate* extra locks by duplicating a same-family template, which
    # inherits its vertex groups, materials, UVs, parent and armature modifier,
    # so the added geometry is skinned and exported exactly like the original.
    "side_lock_extra":  (0.0, 0.0, 2.0, "extra outer side locks per side (rounded to a count)"),
    "side_lock_spread": (0.0, 0.0, 0.26, "lateral offset applied at the tips of the extra side locks"),
    "side_lock_drop":   (0.0, 0.0, 0.16, "extra downward drop of the extra side locks (capped: a tip below the sole would change the total height and rescale every silhouette)"),
    "front_lock_extra": (0.0, 0.0, 2.0, "extra front-falling locks per side (rounded to a count)"),
    "front_lock_fwd":   (0.0, 0.0, 0.26, "forward (-Y) offset of the extra front locks"),
    "front_lock_drop":  (0.0, 0.0, 0.16, "extra downward drop of the extra front locks (capped, see side_lock_drop)"),
    "back_lock_extra":  (0.0, 0.0, 2.0, "extra back locks (rounded to a count)"),
    "back_lock_back":   (0.0, 0.0, 0.30, "backward (+Y) offset of the extra back locks"),
    "back_lock_drop":   (0.0, 0.0, 0.16, "extra downward drop of the extra back locks (capped, see side_lock_drop)"),
}
DEFAULTS: dict[str, float] = {k: v[0] for k, v in PARAM_SPEC.items()}

AHOGE_ROOT = (0.0, 0.10, 2.16)              # from make_ahoge()'s first control point
CROWN_Z0 = 1.92                             # taper starts above the face cap

BACK_LOCKS = ("HairBack0", "HairBack1", "HairBack2", "HairBack3", "HairBack4",
              "HairBackLowL", "HairBackLowR")
SIDE_LOCKS = ("HairSideOuterL", "HairSideOuterR", "HairSideInnerL", "HairSideInnerR")
HAIR_LOCKS = BACK_LOCKS + SIDE_LOCKS
FRINGE = tuple(f"Bang{i}" for i in range(11)) + ("BangSideL", "BangSideR")

# param -> (parts it moves, measured key, required direction of that key,
#           optional z floor: measure only the vertices above it)
PARAM_TARGETS: dict[str, tuple] = {
    "hair_side_flare":  (HAIR_LOCKS, "width_x", +1, None),
    "hair_length":      (HAIR_LOCKS, "min_z", -1, None),
    "hair_back_volume": (BACK_LOCKS, "max_y", +1, None),
    "bang_length":      (FRINGE, "min_z", -1, None),
    "ahoge_scale":      (("Ahoge",), "max_z", +1, None),
    # the skull is widest at its equator, so the taper may only be measured
    # inside the band it actually acts on (and high enough that the ramp weight
    # is non-zero, otherwise the knob reads as inert)
    "crown_taper":      (("Head",), "width_x", -1, 2.00),
    "skirt_flare":      (("Skirt",), "width_x", +1, None),
    "skirt_drop":       (("Skirt",), "min_z", -1, None),
    "foot_length":      (("Shoe.L", "Shoe.R"), "depth_y", +1, None),
    "foot_height":      (("Shoe.L", "Shoe.R"), "height_z", +1, None),
    "tail_flare":       (("TailFlukeA", "TailFlukeB"), "width_x", +1, None),
    # created parts: measured over the template AND its duplicates, so the
    # pattern list spans both prefixes (a prefix that matches nothing would
    # leave the extent undefined)
    "side_lock_extra":  (("HairSideOuter*", "FitAdd_side*"), "width_x", +1, None),
    "side_lock_spread": (("HairSideOuter*", "FitAdd_side*"), "width_x", +1, None),
    "side_lock_drop":   (("HairSideOuter*", "FitAdd_side*"), "min_z", -1, None),
    "front_lock_extra": (("HairSideInner*", "FitAdd_front*"), "depth_y", +1, None),
    "front_lock_fwd":   (("HairSideInner*", "FitAdd_front*"), "min_y", -1, None),
    "front_lock_drop":  (("HairSideInner*", "FitAdd_front*"), "min_z", -1, None),
    "back_lock_extra":  (("HairBack0", "HairBack4", "FitAdd_back*"), "max_y", +1, None),
    "back_lock_back":   (("HairBack0", "HairBack4", "FitAdd_back*"), "max_y", +1, None),
    "back_lock_drop":   (("HairBack0", "HairBack4", "FitAdd_back*"), "min_z", -1, None),
}

# Which model axis every parameter moves vertices along. Front and back cameras
# are both orthographic along Y, so a displacement that lies *entirely* on Y
# projects onto exactly the same pixel before and after: it cannot change either
# silhouette, at any magnitude.
#
# This is not a curiosity, it is the mechanism behind the wig. `hair_back_volume`
# and `back_lock_back` were driven to their limits to fill an ImageGen-guessed
# side profile, and the one drawn view never saw them move by a single pixel --
# so nothing in the objective could object to the character growing a mass of
# hair behind her head. The reverse holds too: a parameter the scored views
# cannot see is not a free variable to be optimised, it is an *unidentifiable*
# one, and the honest move is to freeze it at its factory value rather than let
# a tie-break decide.
PARAM_AXIS: dict[str, str] = {
    "hair_side_flare":  "x",
    "hair_length":      "z",
    "hair_back_volume": "y",
    "bang_length":      "z",
    "ahoge_scale":      "m",            # uniform scale: carries x and z
    "crown_taper":      "x",
    "skirt_flare":      "xy",           # radial push, so the x part is visible
    "skirt_drop":       "z",
    "foot_length":      "y",
    "foot_height":      "z",
    "tail_flare":       "m",
    # Created layers carry their own intrinsic step (LAYER_STEP went into the
    # lateral/forward/back offset, LAYER_DROP into a drop, shrink into all three)
    # so the count is never a pure-Y move even when the offset parameter is.
    "side_lock_extra":  "xz",
    "side_lock_spread": "x",
    "side_lock_drop":   "z",
    "front_lock_extra": "xz",
    "front_lock_fwd":   "y",            # pure -Y: invisible to front and back
    "front_lock_drop":  "z",
    "back_lock_extra":  "xz",
    "back_lock_back":   "y",            # pure +Y: invisible to front and back
    "back_lock_drop":   "z",
}


def axis_visible(axis: str, frozen: set[str]) -> bool:
    """True if a displacement on `axis` reaches a view that is not frozen.

    An orthographic camera along Y is blind to Y only, so "visible" means the
    displacement has any component off the frozen axes. A uniform scale counts
    as visible because it carries x and z.
    """
    return any(c not in frozen for c in axis)


# A parameter that only acts on created layers is *gated* by its count: with
# zero layers the offset has nothing to move. Recording the gate lets both the
# probe and the optimiser open it before judging the offset, instead of
# concluding the offset is inert.
PARAM_COUPLING: dict[str, str] = {
    "side_lock_spread": "side_lock_extra",
    "side_lock_drop":   "side_lock_extra",
    "front_lock_fwd":   "front_lock_extra",
    "front_lock_drop":  "front_lock_extra",
    "back_lock_back":   "back_lock_extra",
    "back_lock_drop":   "back_lock_extra",
}

# Fixed parameters held while probing a gated parameter (the gate at its max).
PARAM_CONTEXT: dict[str, dict] = {
    p: {gate: PARAM_SPEC[gate][2]} for p, gate in PARAM_COUPLING.items()
}


def neutral(params: dict | None = None) -> dict:
    p = dict(DEFAULTS)
    if params:
        p.update(params)
    return p


def is_identity(params: dict | None) -> bool:
    p = neutral(params)
    return all(abs(p[k] - v) <= 1e-12 for k, v in DEFAULTS.items())


# ------------------------------------------------------------------ helpers
def _smooth(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def _world_verts(obj):
    mw = obj.matrix_world
    return [mw @ v.co for v in obj.data.vertices]


def _set_world_verts(obj, pts) -> None:
    inv = obj.matrix_world.inverted()
    for v, p in zip(obj.data.vertices, pts):
        v.co = inv @ Vector(p)      # pts are plain tuples; mathutils needs a Vector
    obj.data.update()


def _ramp(pts, z_root: float, z_tip: float):
    """0 at the root, 1 at the tip, smoothstepped; handles a degenerate span."""
    span = z_root - z_tip
    if abs(span) < 1e-9:
        return [0.0] * len(pts)
    return [_smooth(min(1.0, max(0.0, (z_root - p[2]) / span))) for p in pts]


def _uniform_scale(pts, pivot, s):
    px, py, pz = pivot
    return [(px + (p[0] - px) * s, py + (p[1] - py) * s, pz + (p[2] - pz) * s) for p in pts]


def _centre(pts):
    n = len(pts)
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n, sum(p[2] for p in pts) / n)


# ------------------------------------------------------------------ rule sets
def _deform_hair(obj, p):
    """Outer/side/back locks: flare, lengthen and add back volume at the tips."""
    pts = _world_verts(obj)
    zs = [q[2] for q in pts]
    t = _ramp(pts, max(zs), min(zs))
    flare = p["hair_side_flare"]
    drop = p["hair_length"]
    back = p["hair_back_volume"]
    is_back = obj.name in BACK_LOCKS
    out = []
    for (x, y, z), tt in zip(pts, t):
        if flare:
            x += math.copysign(flare * tt, x) if x else 0.0
        if drop:
            z -= drop * tt
        if back and is_back:
            y += back * tt
        out.append((x, y, z))
    _set_world_verts(obj, out)


def _deform_fringe(obj, p):
    pts = _world_verts(obj)
    zs = [q[2] for q in pts]
    t = _ramp(pts, max(zs), min(zs))
    drop = p["bang_length"]
    _set_world_verts(obj, [(x, y, z - drop * tt) for (x, y, z), tt in zip(pts, t)])


def _deform_ahoge(obj, p):
    pts = _world_verts(obj)
    _set_world_verts(obj, _uniform_scale(pts, AHOGE_ROOT, p["ahoge_scale"]))


def _deform_crown(obj, p):
    """Narrow the skull above the face cap without touching the painted face."""
    pts = _world_verts(obj)
    zs = [q[2] for q in pts]
    z_max = max(zs)
    span = z_max - CROWN_Z0
    k = p["crown_taper"]
    out = []
    for x, y, z in pts:
        if z > CROWN_Z0 and span > 1e-9:
            u = _smooth(min(1.0, (z - CROWN_Z0) / span))
            x *= 1.0 - k * u
        out.append((x, y, z))
    _set_world_verts(obj, out)


def _deform_skirt(obj, p):
    pts = _world_verts(obj)
    zs = [q[2] for q in pts]
    z_top, z_hem = max(zs), min(zs)
    t = _ramp(pts, z_top, z_hem)
    flare = p["skirt_flare"]
    drop = p["skirt_drop"]
    out = []
    for (x, y, z), tt in zip(pts, t):
        r = math.hypot(x, y)
        if flare and r > 1e-9:
            x += flare * tt * x / r
            y += flare * tt * y / r
        if drop:
            z -= drop * tt
        out.append((x, y, z))
    _set_world_verts(obj, out)


def _deform_shoe(obj, p):
    """Shoes are the model's lowest point, so the height pivot is the SOLE.

    Pivoting at the centre would push the sole downwards, and because every
    silhouette is normalised to the same total height that would rescale the
    whole character and cost IoU everywhere (measured: foot_height 2.6 dropped
    the mean IoU from 0.747 to 0.714). Anchoring at the sole makes the knob a
    pure "taller boot" that fills the reference's lower band upward.
    """
    pts = _world_verts(obj)
    cx, cy, _ = _centre(pts)
    z_sole = min(q[2] for q in pts)
    ly, lz = p["foot_length"], p["foot_height"]
    out = [(x, cy + (y - cy) * ly, z_sole + (z - z_sole) * lz) for x, y, z in pts]
    _set_world_verts(obj, out)


def _deform_tail(obj, p):
    pts = _world_verts(obj)
    _set_world_verts(obj, _uniform_scale(pts, _centre(pts), p["tail_flare"]))


# ------------------------------------------------------- part instantiation
ADD_PREFIX = "FitAdd"

FAMILY_TEMPLATES = {
    "side":  ("HairSideOuterL", "HairSideOuterR"),
    "front": ("HairSideInnerL", "HairSideInnerR"),
    "back":  ("HairBack0", "HairBack4"),
}
# Intrinsic spacing of one extra layer (model units) and its downward drop.
# Without this, a bare count creates coincident shells that change nothing.
LAYER_STEP = {"side": 0.075, "front": 0.060, "back": 0.070}
LAYER_DROP = 0.05


def _clear_added() -> int:
    """Drop every object this module created, so apply() is idempotent."""
    removed = 0
    for obj in list(bpy.data.objects):
        if not obj.name.startswith(ADD_PREFIX):
            continue
        mesh = obj.data
        bpy.data.objects.remove(obj, do_unlink=True)
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)
        removed += 1
    return removed


def _duplicate(template: str, name: str):
    src = bpy.data.objects.get(template)
    if src is None or src.type != "MESH":
        return None
    dup = src.copy()                    # copies parent, transform and modifiers
    dup.data = src.data.copy()          # single-user mesh: vertex groups, UVs, materials
    dup.name = name
    dup.data.name = name
    bpy.context.scene.collection.objects.link(dup)
    bpy.context.view_layer.update()
    return dup


def _lock_xform(obj, lateral=0.0, forward=0.0, back=0.0, drop=0.0, shrink=1.0):
    """Root-anchored transform of a lock: the root stays on the scalp, only the
    tip moves. `lateral` is signed outward against the lock's own side."""
    pts = _world_verts(obj)
    zs = [q[2] for q in pts]
    z_root = max(zs)
    t = _ramp(pts, z_root, min(zs))
    n = len(pts)
    cx = sum(q[0] for q in pts) / n
    cy = sum(q[1] for q in pts) / n
    sign = 1.0 if cx >= 0 else -1.0
    out = []
    for (x, y, z), tt in zip(pts, t):
        if shrink != 1.0:
            x = cx + (x - cx) * shrink
            y = cy + (y - cy) * shrink
            z = z_root + (z - z_root) * shrink
        x += sign * lateral * tt
        y += (back - forward) * tt
        z -= drop * tt
        out.append((x, y, z))
    _set_world_verts(obj, out)


def build_added_parts(p: dict) -> list[str]:
    """Instantiate the extra locks implied by the creation parameters.

    Each family spawns `round(count)` extra copies per template, the k-th one
    pushed further out, dropped lower and shrunk slightly so the stack reads as
    a layered mass rather than coincident shells.
    """
    _clear_added()
    created: list[str] = []
    plan = (
        ("side", p["side_lock_extra"], dict(lateral=p["side_lock_spread"]),
         p["side_lock_drop"]),
        ("front", p["front_lock_extra"], dict(forward=p["front_lock_fwd"]),
         p["front_lock_drop"]),
        ("back", p["back_lock_extra"], dict(back=p["back_lock_back"]),
         p["back_lock_drop"]),
    )
    for family, count, offsets, drop in plan:
        n = int(round(count))
        if n <= 0:
            continue
        step = LAYER_STEP[family]
        for k in range(n):
            layer = float(k + 1)            # intrinsic spacing: a bare count
            for template in FAMILY_TEMPLATES[family]:   # must still change the
                name = f"{ADD_PREFIX}_{family}_{template}_{k}"   # silhouette
                obj = _duplicate(template, name)
                if obj is None:
                    continue
                # The k-th extra layer sits one intrinsic step further out and
                # lower than the (k-1)-th, so `*_extra` alone already accumulates
                # volume; the matching offset/drop parameters add on top of that
                # step, which is why their probes run with the count forced up.
                kw = dict(lateral=0.0, forward=0.0, back=0.0)
                if family == "side":
                    kw["lateral"] = (step + offsets.get("lateral", 0.0)) * layer
                elif family == "front":
                    kw["forward"] = (step + offsets.get("forward", 0.0)) * layer
                else:
                    kw["back"] = (step + offsets.get("back", 0.0)) * layer
                _lock_xform(obj, drop=(LAYER_DROP + drop) * layer,
                            shrink=1.0 - 0.07 * k, **kw)
                created.append(name)
    return created


def created_params(p: dict) -> bool:
    return (int(round(p["side_lock_extra"])) or int(round(p["front_lock_extra"]))
            or int(round(p["back_lock_extra"]))) != 0


def apply(params: dict | None = None) -> dict:
    """Deform the scene in place. Returns a report of what actually ran."""
    if bpy is None:
        raise RuntimeError("apply() must run inside Blender")
    p = neutral(params)
    report = {"params": p, "identity": is_identity(params),
              "touched": [], "skipped_neutral": [], "verts": 0, "created": []}

    plan: list[tuple[str, str, callable]] = []
    for name in HAIR_LOCKS:
        plan.append((name, "hair", _deform_hair))
    for name in FRINGE:
        plan.append((name, "fringe", _deform_fringe))
    plan.append(("Ahoge", "ahoge", _deform_ahoge))
    plan.append(("Head", "crown", _deform_crown))
    plan.append(("Skirt", "skirt", _deform_skirt))
    for name in ("Shoe.L", "Shoe.R"):
        plan.append((name, "shoe", _deform_shoe))
    for name in ("TailFlukeA", "TailFlukeB"):
        plan.append((name, "tail", _deform_tail))

    for name, kind, fn in plan:
        obj = bpy.data.objects.get(name)
        if obj is None or obj.type != "MESH":
            continue
        active = {
            "hair": p["hair_side_flare"] != 0.0 or p["hair_length"] != 0.0 or p["hair_back_volume"] != 0.0,
            "fringe": p["bang_length"] != 0.0,
            "ahoge": p["ahoge_scale"] != 1.0,
            "crown": p["crown_taper"] != 0.0,
            "skirt": p["skirt_flare"] != 0.0 or p["skirt_drop"] != 0.0,
            "shoe": p["foot_length"] != 1.0 or p["foot_height"] != 1.0,
            "tail": p["tail_flare"] != 1.0,
        }[kind]
        if not active:
            report["skipped_neutral"].append(name)
            continue
        fn(obj, p)
        report["touched"].append(name)
        report["verts"] += len(obj.data.vertices)

    # Creation runs last so the extra layers inherit whatever the deform pass
    # did to their templates (a flared base layer gives flared extra layers).
    report["created"] = build_added_parts(p)
    for name in report["created"]:
        obj = bpy.data.objects.get(name)
        if obj is not None:
            report["verts"] += len(obj.data.vertices)
    return report


def capture() -> dict:
    """Snapshot every mesh part's world-space vertex positions.

    Needed because apply() must NOT undo itself: the rules are skipped when a
    parameter is neutral (that is what makes the identity gate exact), so
    `apply(DEFAULTS)` is a no-op rather than a rollback. Multi-sample probing
    therefore restores from a snapshot instead of relying on a reset call.
    """
    if bpy is None:
        raise RuntimeError("capture() must run inside Blender")
    snap = {}
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        mw = obj.matrix_world
        snap[obj.name] = [tuple(mw @ v.co) for v in obj.data.vertices]
    return snap


def restore(snap: dict) -> None:
    """Put the scene back exactly as capture() found it."""
    _clear_added()
    for name, pts in snap.items():
        obj = bpy.data.objects.get(name)
        if obj is None or obj.type != "MESH":
            continue
        _set_world_verts(obj, pts)


def probe(param: str, factors=(0.0, 0.5, 1.0)) -> dict:
    """Sweep one parameter over its range and measure the parts it targets.

    Measuring the whole scene would drown the signal (the fringe tips sit far
    inside the head silhouette), so each parameter declares the parts it moves
    and the axis that must respond monotonically. `direction` is +1 when the
    measured key must not decrease as the parameter rises, -1 when it must not
    increase.
    """
    if bpy is None:
        raise RuntimeError("probe() must run inside Blender")
    names, key, direction, z_floor = PARAM_TARGETS[param]
    lo, hi = PARAM_SPEC[param][1], PARAM_SPEC[param][2]
    # parameters that only act on created layers are probed in the context
    # where they exist: sweeping a "how far out" knob with the count at zero
    # would read as inert for the wrong reason
    snap = capture()
    rows = []
    context = PARAM_CONTEXT.get(param, {})
    for f in factors:
        restore(snap)                       # each sample starts from the shipped mesh
        p = dict(DEFAULTS)
        p.update(context)
        p[param] = lo + (hi - lo) * f
        apply(p)
        mins = [1e9] * 3
        maxs = [-1e9] * 3
        matched = 0
        for name in names:
            if name.endswith("*"):          # dynamic names: created parts
                targets = [o for o in bpy.data.objects
                           if o.name.startswith(name[:-1]) and o.type == "MESH"]
            else:
                obj = bpy.data.objects.get(name)
                targets = [obj] if obj is not None and obj.type == "MESH" else []
            for obj in targets:
                matched += 1
                mw = obj.matrix_world
                for v in obj.data.vertices:
                    w = mw @ v.co
                    if z_floor is not None and w.z < z_floor:
                        continue
                    for i in range(3):
                        mins[i] = min(mins[i], w[i])
                        maxs[i] = max(maxs[i], w[i])
        rows.append({"factor": f, "value": round(p[param], 6), "parts": matched,
                     "width_x": round(maxs[0] - mins[0], 6),
                     "depth_y": round(maxs[1] - mins[1], 6),
                     "height_z": round(maxs[2] - mins[2], 6),
                     "min_y": round(mins[1], 6), "max_y": round(maxs[1], 6),
                     "min_z": round(mins[2], 6), "max_z": round(maxs[2], 6)})
    restore(snap)
    series = [r[key] for r in rows]
    ok = all(b - a >= -1e-9 if direction > 0 else b - a <= 1e-9
             for a, b in zip(series, series[1:]))
    moved = abs(series[-1] - series[0]) > 1e-6
    return {"param": param, "objects": list(names), "key": key,
            "direction": direction, "rows": rows, "monotone": ok,
            "responds": moved, "context": context,
            "delta": round(series[-1] - series[0], 6)}


def describe() -> dict:
    return {"params": {k: {"default": v[0], "min": v[1], "max": v[2], "doc": v[3]}
                       for k, v in PARAM_SPEC.items()},
            "parts": {"hair_locks": list(HAIR_LOCKS), "fringe": list(FRINGE)}}
