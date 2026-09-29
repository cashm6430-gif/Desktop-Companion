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
    "ParamMouthOpenY", "ParamArmLA", "ParamArmRA",
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
    print(f"Validated {len(paths)} Cubism files and {len(parameters)} parameters")


if __name__ == "__main__":
    main()
