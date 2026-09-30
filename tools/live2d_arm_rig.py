"""Canvas-space arm kinematics expressed as PSD2Live public operations.

The editor stores vertices in parent-local units. Compensate for each parent's
aspect before rotations so separate sleeves, hands and grass share a grip.
"""
import math


def arm_operations(bounds, aspect, side, arm, elbow=0, reach=0, kind="forearm", swing=0, tip=0):
    right = side == "r"
    shoulder = (758, 643) if right else (496, 643)
    joint = (765, 720) if right else (489, 720)
    wrist = (855, 802) if right else (399, 802)
    direction = -1 if right else 1

    def normalized(point):
        x = shoulder[0] + aspect * (point[0] - shoulder[0])
        return [(x - bounds[0]) / bounds[2], (point[1] - bounds[1]) / bounds[3]]

    pivot = [(shoulder[0] - bounds[0]) / bounds[2], (shoulder[1] - bounds[1]) / bounds[3]]
    operations = [{"type": "scale", "pivot": pivot, "factors": [aspect, 1]}]
    if kind == "grass":
        if tip:
            operations.append({"type": "arc", "root": normalized((884, 669)),
                "tip": normalized((806, 498)), "root_pin": 0.08, "degrees": tip * 32})
        if swing:
            operations.append({"type": "arc", "root": normalized((913, 852)),
                "tip": normalized((806, 498)), "root_pin": 0.12, "degrees": swing * 65})
        if reach:
            operations.append({"type": "rotate", "pivot": normalized((913, 852)), "degrees": reach * 30})
    if kind != "upper":
        if reach:
            # The distal sleeve broadens while its projected length shrinks.
            # The palm grows separately about the same cuff, not the shoulder.
            length = 1 - 0.42 * reach
            if kind == "forearm":
                axis = math.degrees(math.atan2(wrist[1] - joint[1], wrist[0] - joint[0]))
                operations.extend([
                    {"type": "rotate", "pivot": normalized(joint), "degrees": -axis},
                    {"type": "scale", "pivot": normalized(joint), "factors": [length, 1 + 0.52 * reach]},
                    {"type": "rotate", "pivot": normalized(joint), "degrees": axis},
                ])
            else:
                operations.extend([
                    {"type": "scale", "pivot": normalized(wrist), "factors": [1 + 0.52 * reach] * 2},
                    {"type": "translate", "delta": [aspect * (length - 1) * (wrist[0] - joint[0]) / bounds[2],
                        (length - 1) * (wrist[1] - joint[1]) / bounds[3]]},
                ])
        operations.append({"type": "rotate", "pivot": normalized(joint), "degrees": elbow * direction})
    rotation = {"type": "rotate", "pivot": pivot, "degrees": arm * direction}
    if kind == "upper":
        # Rotation is exact at the elbow and fades around the puff's shoulder.
        rotation["selection"] = {"center": [(joint[0] - bounds[0]) / bounds[2],
            (joint[1] - bounds[1]) / bounds[3]], "radius": 0.85, "hardness": 0.65}
    operations.append(rotation)
    operations.append({"type": "scale", "pivot": pivot, "factors": [1 / aspect, 1]})
    return operations
