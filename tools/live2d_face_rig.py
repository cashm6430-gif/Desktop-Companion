"""Cheek shaping keys expressed as PSD2Live public operations.

The face is one plate under DeformFaceContour. The approved master already puts
the hair over the outer cheeks, so a silhouette change would fight the reviewed
art. Bulging the plate outward from its centre therefore only rounds the skin
that is actually exposed, which is the reading we want for a mouth full of food.
"""

# Canvas landmarks measured on the reviewed 1254 master. The eyes end near
# y=510 and the mouth sits at x 605..652, so the bulges are placed below and
# outside both and never touch the painting they would need to cover.
LEFT_CHEEK = (505.0, 558.0)
RIGHT_CHEEK = (752.0, 558.0)
CHEEK_RADIUS = 104.0
CHEEK_OUT = 34.0
CHEEK_DOWN = 4.0


def cheek_operations(bounds, aspect, amount=1.0):
    x, y, w, h = bounds

    def key(point):
        return [(point[0] - x) / w, (point[1] - y) / h]

    pivot = [0.5, 0.5]
    out = CHEEK_OUT * amount / w
    down = CHEEK_DOWN * amount / h
    # Selection and pivot are normalised against the fixed input bounds, so the
    # aspect pair is what keeps a scalar radius round instead of elliptical.
    # Everything between the two scale operations runs in that square space,
    # which is also why the outward delta carries the aspect back.
    selection = {"radius": CHEEK_RADIUS / w, "hardness": 0.5}
    return [
        {"type": "scale", "pivot": pivot, "factors": [aspect, 1]},
        {"type": "translate", "delta": [-out * aspect, down],
         "selection": dict(selection, center=key(LEFT_CHEEK))},
        {"type": "translate", "delta": [out * aspect, down],
         "selection": dict(selection, center=key(RIGHT_CHEEK))},
        {"type": "scale", "pivot": pivot, "factors": [1 / aspect, 1]},
    ]
