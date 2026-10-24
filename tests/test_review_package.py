"""Manifest validation must reject incomplete or altered delivery artifacts."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "review_package", Path(__file__).resolve().parents[1] / "tools/review_package.py")
review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(review)


class PackageInventoryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="pet-package-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.stage = self.root / "stage"
        self.stage.mkdir()
        self.names = review.REQUIRED | {"FrameworkShaders/fixture.frag", "assets/live2d/whale-girl/fixture.png"}
        for name in self.names:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(name.encode("utf8"))
        (self.stage / review.MODEL).write_text(json.dumps({"FileReferences": {
            "Moc": "whale-girl-layered-draft.moc3", "Textures": ["fixture.png"]}}), encoding="utf8")
        self.manifest = {"schema_version": 1, "files": [
            {"path": name, "bytes": (self.stage / name).stat().st_size,
             "sha256": review.sha256(self.stage / name)} for name in sorted(self.names)]}
        self.write_manifest()

    def write_manifest(self):
        (self.stage / review.MANIFEST).write_text(json.dumps(self.manifest), encoding="utf8")

    def test_stage_and_portable_have_exact_same_bytes(self):
        _, files = review.verify_stage(self.stage)
        archive = self.root / "pet.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for name in self.names | {review.MANIFEST}:
                output.write(self.stage / name, "DesktopCompanion/" + name)
        review.verify_portable(archive, self.stage, files)

    def test_changed_same_length_file_is_rejected(self):
        path = self.stage / "DesktopCompanion.exe"
        path.write_bytes(b"X" * path.stat().st_size)
        with self.assertRaisesRegex(ValueError, "bytes disagree"):
            review.verify_stage(self.stage)

    def test_missing_and_extra_files_are_rejected(self):
        path = self.stage / "Qt6Core.dll"
        path.unlink()
        with self.assertRaises(ValueError):
            review.verify_stage(self.stage)
        path.write_bytes(b"Qt6Core.dll")
        (self.stage / "unexpected.dll").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "unlisted"):
            review.verify_stage(self.stage)

    def test_required_file_cannot_be_omitted_from_both_manifest_and_tree(self):
        name = "tools/hook-config-common.ps1"
        (self.stage / name).unlink()
        self.manifest["files"] = [entry for entry in self.manifest["files"] if entry["path"] != name]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Required runtime"):
            review.verify_stage(self.stage)

    def test_missing_model_texture_is_rejected_even_with_matching_inventory(self):
        name = "assets/live2d/whale-girl/fixture.png"
        (self.stage / name).unlink()
        self.manifest["files"] = [entry for entry in self.manifest["files"] if entry["path"] != name]
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Model reference missing"):
            review.verify_stage(self.stage)

    def test_motion_metadata_and_crt_are_required(self):
        for name in ("assets/motions/busy-rest.motion.json",
                     "assets/live2d/whale-girl/whale-girl-layered-draft.psd2live.json",
                     "msvcp140_2.dll"):
            entry = next(item for item in self.manifest["files"] if item["path"] == name)
            self.manifest["files"].remove(entry)
            data = (self.stage / name).read_bytes()
            (self.stage / name).unlink()
            self.write_manifest()
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Required runtime"):
                review.verify_stage(self.stage)
            (self.stage / name).write_bytes(data)
            self.manifest["files"].append(entry)

    def test_paths_and_case_duplicates_are_rejected(self):
        for value in ("../escape", "/absolute", "C:/outside", "tools\\outside", "a/./b", "a\x00b"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                review.safe_relative(value)
        self.manifest["files"].append(dict(self.manifest["files"][0]))
        self.write_manifest()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            review.verify_stage(self.stage)

    def test_portable_altered_bytes_are_rejected(self):
        _, files = review.verify_stage(self.stage)
        archive = self.root / "altered.zip"
        with zipfile.ZipFile(archive, "w") as output:
            for name in self.names | {review.MANIFEST}:
                data = (self.stage / name).read_bytes()
                output.writestr("DesktopCompanion/" + name,
                                b"X" * len(data) if name == "DesktopCompanion.exe" else data)
        with self.assertRaisesRegex(ValueError, "Portable bytes mismatch"):
            review.verify_portable(archive, self.stage, files)


if __name__ == "__main__":
    unittest.main()
