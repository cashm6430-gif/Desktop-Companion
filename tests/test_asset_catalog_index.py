"""The staged checkpoint must not absorb unrelated assets or unsaved bytes."""

import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "asset_catalog", Path(__file__).resolve().parents[1] / "tools/verify_asset_catalog.py")
CATALOG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CATALOG)


class AssetCatalogIndexTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        self.write("art/live2d/workflow/asset-catalog.json", b"{}\n")
        self.write("assets/3d/prototype/character.glb", b"staged model\x00\xff")
        self.write("godot/toon-prototype/main.gd", b"extends Node3D\n")
        self.git("add", ".")

    def tearDown(self):
        self.temporary.cleanup()

    def git(self, *arguments):
        return subprocess.run(["git", *arguments], cwd=self.root, check=True,
                              capture_output=True)

    def write(self, name, payload):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)

    def test_staged_checkpoint_ignores_untracked_and_uses_staged_bytes(self):
        self.write("assets/3d/prototype/character.glb", b"unsaved changed model")
        self.write("tools/unrelated_user_work.py", b"user draft\n")
        result = CATALOG.refresh(self.root, ":")
        self.assertEqual(result["mode"], "refresh_index")
        data = json.loads((self.root / CATALOG.CATALOG).read_text(encoding="utf8"))
        self.assertNotIn("tools/unrelated_user_work.py", data["files"])
        self.assertEqual(data["files"]["assets/3d/prototype/character.glb"]["sha256"],
                         hashlib.sha256(b"staged model\x00\xff").hexdigest())
        self.git("add", CATALOG.CATALOG)
        self.assertTrue(CATALOG.verify(self.root, ":")["passed"])
        self.assertFalse(CATALOG.verify(self.root)["passed"])

    def test_godot_source_is_protected_by_staged_catalog(self):
        CATALOG.refresh(self.root, ":")
        self.git("add", CATALOG.CATALOG)
        self.write("godot/toon-prototype/main.gd", b"extends Node3D\n# changed\n")
        self.git("add", "godot/toon-prototype/main.gd")
        result = CATALOG.verify(self.root, ":")
        self.assertFalse(result["passed"])
        self.assertIn("godot/toon-prototype/main.gd",
                      {item["path"] for item in result["failures"]})

    def test_worktree_refresh_keeps_explicit_existing_behavior(self):
        self.write("assets/new-reference.png", b"new reference")
        CATALOG.refresh(self.root)
        data = json.loads((self.root / CATALOG.CATALOG).read_text(encoding="utf8"))
        self.assertIn("assets/new-reference.png", data["files"])
        self.assertTrue(CATALOG.verify(self.root)["passed"])


if __name__ == "__main__":
    unittest.main()
