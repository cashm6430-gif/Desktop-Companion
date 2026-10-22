"""Check the PSD2Live export before packaging the desktop pet."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import platform
import sys


ROOT = Path(__file__).resolve().parents[1]
MODEL_DIR = ROOT / "assets/live2d/whale-girl"
MODEL = MODEL_DIR / "whale-girl-layered-draft.model3.json"
REQUIRED_PARAMETERS = {
    "ParamAngleX", "ParamAngleY", "ParamAngleZ",
    "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
    "ParamEyeLOpen", "ParamEyeROpen", "ParamBreath",
    "ParamMouthOpenY", "ParamMouthForm", "ParamArmLA", "ParamArmRA",
    "ParamCheek", "ParamMouthGape",
    "ParamGrassVisible", "ParamGrassReach", "ParamGrassSwing",
    "ParamGrassTipBend",
    "ParamHandRGrip",
    "ParamSmileOpen", "ParamEyeBallX", "ParamEyeBallY",
    "ParamEyeSmile",
    "ParamElbowLA", "ParamElbowRA",
    "ParamWristRA",
    "ParamBusyLaptop", "ParamBusyTypingL", "ParamBusyTypingR", "ParamLaptopRock",
    "ParamSitPose", "ParamLaptopVisible",
    # Whole-rig horizontal translation on the outermost ShiftRig warp:
    # +X moves right, |100| ~ 115 source px (~26 screen px). ParamShiftY is
    # declared too but cancelled by the renderer's ground contract.
    "ParamShiftX", "ParamShiftY",
    # The thinking pose drives the hair layers as secondary motion.
    "ParamHairFront", "ParamHairBack",
}

# Declared by the model, and driven by nothing that matters: pushing either to
# its limit and diffing the render is byte identical, because the art has no
# eyebrows drawn and no mesh carries a key on them. A clip that animates one of
# these looks perfectly reasonable in review and moves exactly nothing on
# screen, which is why it fails here instead of shipping. Re-derive this set by
# posing each declared axis at its min and max against a neutral pose (the
# skill's "which axes are alive" recipe) whenever the artwork changes.
INERT_PARAMETERS = {"ParamBrowLY", "ParamBrowRY"}

# Bound to the ShiftRig warp, but CubismCanvas's ground contract presses the
# lowest standing foot vertex back onto the floor every frame, cancelling any
# whole-model vertical travel before it reaches the screen. Unlike the inert
# brows this axis does carry keys, so the "keyed by no drawable" check cannot
# catch it; reject it by name instead.
GROUNDED_PARAMETERS = {"ParamShiftY"}


def native_parameter_ids(moc_path, core_path=None):
    """Read IDs from this MOC, not the optional/stale display-info labels.

    Signatures and alignments follow the bundled Core/include/
    Live2DCubismCore.h: csmAlignofMoc=64, csmAlignofModel=16, unsigned-int
    buffer sizes, opaque pointers, and const char** parameter IDs.
    """
    if core_path is None:
        if sys.platform == "win32":
            arch = "x86_64" if ctypes.sizeof(ctypes.c_void_p) == 8 else "x86"
            candidates = [ROOT / "build/Live2DCubismCore.dll",
                          ROOT / f"CubismSdkForNative-5-r.5/Core/dll/windows/{arch}/Live2DCubismCore.dll"]
        elif sys.platform == "darwin":
            candidates = [ROOT / "build/libLive2DCubismCore.dylib",
                          ROOT / "CubismSdkForNative-5-r.5/Core/dll/macos/libLive2DCubismCore.dylib"]
        else:
            candidates = [ROOT / "build/libLive2DCubismCore.so",
                          ROOT / f"CubismSdkForNative-5-r.5/Core/dll/linux/{platform.machine().lower()}/libLive2DCubismCore.so"]
        core_path = next((path for path in candidates if path.is_file()), None)
        if core_path is None:
            raise ValueError("Cubism Native Core is unavailable; cannot verify motion parameters "
                             "missing from DisplayInfo against the actual MOC. "
                             "Provide the matching local Core library or SDK.")
    try:
        # Windows DLL exports use __stdcall (the x64 ABI is shared).
        loader = ctypes.WinDLL if sys.platform == "win32" else ctypes.CDLL
        core = loader(str(core_path))
        pointer = ctypes.c_void_p

        def bind(name, result, arguments):
            function = getattr(core, name)
            function.restype, function.argtypes = result, arguments
            return function

        consistent = bind("csmHasMocConsistency", ctypes.c_int, [pointer, ctypes.c_uint])
        revive = bind("csmReviveMocInPlace", pointer, [pointer, ctypes.c_uint])
        model_size = bind("csmGetSizeofModel", ctypes.c_uint, [pointer])
        initialize = bind("csmInitializeModelInPlace", pointer, [pointer, pointer, ctypes.c_uint])
        count_parameters = bind("csmGetParameterCount", ctypes.c_int, [pointer])
        parameter_ids = bind("csmGetParameterIds", ctypes.POINTER(ctypes.c_char_p), [pointer])
        raw = Path(moc_path).read_bytes()
        if len(raw) < 64 or len(raw) > 0xffffffff or not raw.startswith(b"MOC3"):
            raise ValueError(f"Not a complete MOC3 file: {moc_path}")

        def aligned_buffer(size, alignment):
            storage = ctypes.create_string_buffer(size + alignment - 1)
            address = (ctypes.addressof(storage) + alignment - 1) & ~(alignment - 1)
            return storage, address

        # Keep both backing buffers alive until every Native ID is copied.
        moc_storage, moc_address = aligned_buffer(len(raw), 64)
        ctypes.memmove(moc_address, raw, len(raw))
        if consistent(moc_address, len(raw)) != 1:
            raise ValueError(f"Cubism Core rejected MOC consistency: {moc_path}")
        moc = revive(moc_address, len(raw))
        size = model_size(moc) if moc else 0
        if not size:
            raise ValueError(f"Cubism Core could not revive MOC: {moc_path}")
        model_storage, model_address = aligned_buffer(size, 16)
        model = initialize(moc, model_address, size)
        if not model:
            raise ValueError(f"Cubism Core could not initialize MOC: {moc_path}")
        count, ids = count_parameters(model), parameter_ids(model)
        if count <= 0 or count > 100_000 or not ids:
            raise ValueError("Cubism Core returned no valid parameter ID table")
        result = {ids[index].decode("utf8") for index in range(count) if ids[index]}
        if len(result) != count or "" in result:
            raise ValueError("Cubism Core returned empty or duplicate parameter IDs")
        return result
    except (OSError, AttributeError, UnicodeError) as error:
        raise ValueError(f"Cannot verify actual MOC parameter IDs with {core_path}: {error}") from error


def validate_parameter_bindings(moc_path, display_parameters, motions, core_path=None):
    """Validate name -> used-ID sets; Native IDs are authoritative if needed.

    DisplayInfo-only validation remains usable offline when it covers every
    motion. An extra motion ID requires real Core verification, never a
    runtime-parameter allowlist or a union with potentially stale CDI IDs.
    """
    parameters = set(display_parameters)
    used = set().union(*motions.values()) if motions else set()
    if used - parameters:
        parameters = native_parameter_ids(moc_path, core_path)
    for name, identifiers in motions.items():
        unbound = sorted(set(identifiers) - parameters)
        if unbound:
            raise ValueError(f"{name} uses unbound model parameters: {unbound}")
    return parameters


def validate_seated_bindings(metadata, moc_path):
    """Keep all eight legacy bindings and verify the adopted desk replacements."""
    layers = metadata["layers"]
    legacy = {
        "busy torso": "ParamBusyLaptop", "busy skirt": "ParamBusyLaptop",
        "busy leg r": "ParamBusyLaptop", "busy leg l": "ParamBusyLaptop",
        "busy laptop base": "ParamLaptopVisible", "busy laptop lid": "ParamLaptopVisible",
        "busy sleeve l": "ParamBusyLaptop", "busy sleeve r": "ParamBusyLaptop",
    }

    def bound_layer(source, parameter, kind, drawable=None):
        matches = [layer for layer in layers if layer.get("source") == source]
        if len(matches) != 1:
            raise ValueError(f"Seated binding requires one layer with source {source!r}")
        layer = matches[0]
        if (layer.get("parameter") != parameter or layer.get("type") != kind
                or not isinstance(layer.get("drawable"), str) or not layer["drawable"]
                or (drawable is not None and layer["drawable"] != drawable)):
            raise ValueError(f"Invalid seated source/parameter/type/drawable binding: {source}")
        return layer

    legacy_layers = {source: bound_layer(source, parameter, "toggle")
                     for source, parameter in legacy.items()}
    desk = metadata.get("runtimeDeskWorkMode")
    if (not isinstance(desk, dict) or type(desk.get("version")) is not int or desk.get("version") != 1
            or desk.get("stage") != "complete" or desk.get("adopted") is not True
            or desk.get("parameter") != "ParamDeskVisible"
            or desk.get("floorOwner") != "runtime" or desk.get("hiddenLeftHand") is not True):
        raise ValueError("Adopted runtimeDeskWorkMode requires complete version 1 bindings")
    if desk.get("mocSha256") != hashlib.sha256(Path(moc_path).read_bytes()).hexdigest():
        raise ValueError("runtimeDeskWorkMode MOC hash does not match the actual model")
    laptop_id = desk.get("replacementLaptop")
    if not isinstance(laptop_id, str) or not laptop_id:
        raise ValueError("runtimeDeskWorkMode requires its replacementLaptop drawable")
    right_sleeve_id = legacy_layers["busy sleeve r"]["drawable"]
    replacements = desk.get("replacementSleeves")
    if (not isinstance(replacements, dict) or set(replacements) != {right_sleeve_id}
            or not isinstance(replacements[right_sleeve_id], str) or not replacements[right_sleeve_id]):
        raise ValueError("runtimeDeskWorkMode replacementSleeves must map the legacy right sleeve")
    bound_layer("busy workstation laptop", "ParamLaptopVisible", "scene-prop", laptop_id)
    bound_layer("busy workstation sleeve r", "ParamBusyLaptop", "toggle", replacements[right_sleeve_id])
    desk_ids = desk.get("drawableIds")
    if (not isinstance(desk_ids, list) or len(desk_ids) != 2
            or any(not isinstance(identifier, str) or not identifier for identifier in desk_ids)
            or len(set(desk_ids)) != 2):
        raise ValueError("runtimeDeskWorkMode requires two distinct desk drawables")
    for identifier in desk_ids:
        bound_layer(f"workstation {identifier}", "ParamDeskVisible", "scene-prop", identifier)
    expected_busy_sources = set(legacy) | {"busy workstation laptop", "busy workstation sleeve r"}
    seated = [layer for layer in layers if layer.get("source", "").startswith("busy ")]
    if (len(seated) != len(expected_busy_sources)
            or {layer["source"] for layer in seated} != expected_busy_sources):
        raise ValueError("Unexpected or missing seated pieces outside the declared legacy/replacement bindings")
    declared_ids = [layer["drawable"] for layer in legacy_layers.values()]
    declared_ids += [laptop_id, replacements[right_sleeve_id], *desk_ids]
    if len(set(declared_ids)) != len(declared_ids):
        raise ValueError("Seated legacy, replacement and desk drawables must be distinct")


def main(argv=None) -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--motion", action="append", metavar="CLIP_ID",
                        help="Check only this authored motion's curves; repeat for multiple clips. "
                             "Model references and binding structure are always checked in full.")
    args = parser.parse_args(argv)
    available = {path.name.removesuffix(".motion.json"): path
                 for path in sorted((ROOT / "assets/motions").glob("*.motion.json"))}
    selected = available
    if args.motion is not None:
        unknown = sorted(set(args.motion) - available.keys())
        if unknown:
            parser.error(f"Unknown motion IDs: {unknown}; available IDs: {sorted(available)}")
        selected = {name: available[name] for name in sorted(set(args.motion))}
    data = json.loads(MODEL.read_text(encoding="utf-8"))
    refs = data["FileReferences"]
    paths = [refs["Moc"], refs["Physics"], refs["DisplayInfo"]]
    paths += refs["Textures"]
    paths += [motion["File"] for group in refs["Motions"].values()
              for motion in group]
    missing = [path for path in paths if not (MODEL_DIR / path).is_file()]
    if missing:
        raise SystemExit(f"Missing model references: {missing}")
    info = json.loads((MODEL_DIR / refs["DisplayInfo"]).read_text(encoding="utf-8"))
    parameters = {item["Id"] for item in info["Parameters"]}
    # Every authored motion, whatever curve format it uses, must only drive
    # parameters the model actually rigs.
    motions = {}
    for path in selected.values():
        motion = json.loads(path.read_text(encoding="utf8"))
        used = {key for frame in motion.get("keyframes", []) for key in frame["parameters"]}
        used |= set(motion.get("tracks", {}))
        used |= set(motion.get("constants", {}))
        used |= set(motion.get("channels", {}))
        used |= set(motion.get("pulses", {}))
        motions[path.name] = used
        inert = sorted(used & INERT_PARAMETERS)
        if inert:
            raise SystemExit(
                f"{path.name} drives parameters keyed by no drawable: {inert}. "
                "Those curves render no pixels; drop them or give them art.")
        grounded = sorted(used & GROUNDED_PARAMETERS)
        if grounded:
            raise SystemExit(
                f"{path.name} keys vertical whole-model travel: {grounded}. "
                "CubismCanvas grounds the standing feet every frame, so "
                "ParamShiftY renders no motion; use ParamShiftX or rework the "
                "ground contract first.")
    try:
        parameters = validate_parameter_bindings(MODEL_DIR / refs["Moc"], parameters, motions)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    missing_parameters = sorted(REQUIRED_PARAMETERS - parameters)
    if missing_parameters:
        raise SystemExit(f"Missing rigged parameters: {missing_parameters}")
    metadata = json.loads((MODEL_DIR / "whale-girl-layered-draft.psd2live.json").read_text(encoding="utf8"))
    prop = next((layer for layer in metadata["layers"] if layer["source"] == "handwear right"), None)
    if not prop or prop.get("parameter") != "ParamGrassVisible":
        raise SystemExit("Grass prop must be exported as a parameter-driven drawable")
    for source, switch_id in (("handwear_r", 0), ("handwear.right", 1)):
        hand = next((layer for layer in metadata["layers"] if layer["source"] == source), None)
        if not hand or hand.get("parameter") != "ParamHandRGrip" or hand.get("type") != "switch" or hand.get("switchId") != switch_id:
            raise SystemExit("Open and gripping hands must share complementary native opacity keys")
    for source, switch_id in (("mouth", 0), ("mouth open", 1)):
        mouth = next((layer for layer in metadata["layers"] if layer["source"] == source), None)
        if not mouth or mouth.get("parameter") != "ParamSmileOpen" or mouth.get("switchId") != switch_id:
            raise SystemExit("Closed smile and open laugh must be bound to native expression keys")
    if any(layer["source"] == "face detail backing" for layer in metadata["layers"]):
        raise SystemExit("Hard oval eye backing must not be exported")
    for source in ("irides-l", "irides-r", "eyewhite-l", "eyewhite-r", "eyelash-l", "eyelash-r"):
        eye = next((layer for layer in metadata["layers"] if layer["source"] == source), None)
        if not eye or eye.get("type") != "preset":
            raise SystemExit("Eyes must close geometrically, not crossfade their source art")
    for source in ("upperarm-l", "upperarm-r", "handwear-l", "handwear-r"):
        sleeve = next((layer for layer in metadata["layers"] if layer["source"] == source), None)
        if not sleeve or sleeve.get("tag") != "handwear":
            raise SystemExit("Upper and lower sleeves must be independent handwear layers: " + source)
    order = {layer["source"]: index for index, layer in enumerate(metadata["layers"])}
    try:
        validate_seated_bindings(metadata, MODEL_DIR / refs["Moc"])
    except ValueError as error:
        raise SystemExit(str(error)) from error
    if not all(order[arm] > order[face] for arm in ("handwear-l", "handwear-r", "handwear_r")
               for face in ("face", "front hair", "eyelash-l", "eyelash-r", "irides-l", "irides-r")):
        raise SystemExit("Distal limbs must cover the face when reaching across it")
    print(f"Validated {len(paths)} Cubism files, {len(parameters)} parameters "
          f"and {len(selected)} authored motions ({', '.join(selected)})")


if __name__ == "__main__":
    main()
