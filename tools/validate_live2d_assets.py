"""Check the PSD2Live export before packaging the desktop pet."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODEL_DIR = ROOT / "assets/live2d/whale-girl"
MODEL = MODEL_DIR / "whale-girl-layered-draft.model3.json"
REQUIRED_PARAMETERS = {
    "ParamAngleX", "ParamAngleY", "ParamAngleZ",
    "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
    "ParamEyeLOpen", "ParamEyeROpen", "ParamBreath",
    "ParamMouthOpenY", "ParamMouthForm", "ParamArmLA", "ParamArmRA",
    "ParamCheek",
    "ParamGrassVisible", "ParamGrassReach", "ParamGrassSwing",
    "ParamGrassTipBend",
    "ParamHandRGrip",
    "ParamSmileOpen", "ParamEyeBallX", "ParamEyeBallY",
    "ParamEyeSmile",
    "ParamElbowLA", "ParamElbowRA",
    "ParamWristRA",
    "ParamBusyLaptop", "ParamBusyTypingL", "ParamBusyTypingR", "ParamLaptopRock",
    "ParamSitPose", "ParamLaptopVisible",
}


def main() -> None:
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
    missing_parameters = sorted(REQUIRED_PARAMETERS - parameters)
    if missing_parameters:
        raise SystemExit(f"Missing rigged parameters: {missing_parameters}")
    # Every authored motion, whatever curve format it uses, must only drive
    # parameters the model actually rigs.
    for path in sorted((ROOT / "assets/motions").glob("*.motion.json")):
        motion = json.loads(path.read_text(encoding="utf8"))
        used = {key for frame in motion.get("keyframes", []) for key in frame["parameters"]}
        used |= set(motion.get("tracks", {}))
        used |= set(motion.get("constants", {}))
        used |= set(motion.get("channels", {}))
        used |= set(motion.get("pulses", {}))
        unbound = sorted(used - parameters)
        if unbound:
            raise SystemExit(f"{path.name} uses unbound model parameters: {unbound}")
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
    seated = [layer for layer in metadata['layers'] if layer['source'].startswith('busy ')]
    if len(seated) != 8 or any(layer.get('parameter') != ('ParamLaptopVisible' if layer['source'].startswith('busy laptop') else 'ParamBusyLaptop') or layer.get('type') != 'toggle' for layer in seated):
        raise SystemExit('Eight separate seated pieces must have native busy visibility bindings')
    if not all(order[arm] > order[face] for arm in ("handwear-l", "handwear-r", "handwear_r")
               for face in ("face", "front hair", "eyelash-l", "eyelash-r", "irides-l", "irides-r")):
        raise SystemExit("Distal limbs must cover the face when reaching across it")
    print(f"Validated {len(paths)} Cubism files and {len(parameters)} parameters")


if __name__ == "__main__":
    main()
