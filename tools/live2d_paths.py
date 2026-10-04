"""Stable authoring inputs and disposable model export locations."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAYERS = ROOT / "art/live2d/layers"
STATE = ROOT / ".local/authoring/state.json"
EXPORT = ROOT / "build/model-export"
DEPENDENCIES = ROOT / ".local/python-authoring/site-packages"


def enable_authoring_dependencies():
    """Use the preserved local packages only with their matching Python ABI."""
    marker = DEPENDENCIES.parent / "interpreter.json"
    if DEPENDENCIES.is_dir() and marker.is_file():
        runtime = json.loads(marker.read_text(encoding="utf8"))
        if runtime.get("cache_tag") == sys.implementation.cache_tag:
            sys.path.insert(0, str(DEPENDENCIES))
