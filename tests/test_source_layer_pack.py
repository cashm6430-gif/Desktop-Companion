"""Regression checks for exact alpha, source drift and isolated pack output."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_source_layer_pack as pack
from PIL import Image
from psd_tools import PSDImage


class SourceLayerPackTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.psd_path = self.source / "original.psd"
        self.audit_path = self.source / "audit.json"
        self.output = self.root / "pack"
        size = (16, 12)
        low_alpha = Image.new("RGBA", (3, 2), (14, 72, 121, 2))
        low_alpha.putpixel((1, 0), (219, 190, 145, 255))
        opaque = Image.new("RGBA", (2, 3), (30, 65, 190, 255))
        psd = PSDImage.new("RGBA", size=size, color=(0, 0, 0, 0))
        psd.create_pixel_layer(low_alpha, name="bite mouth", left=5, top=4)
        psd.create_pixel_layer(opaque, name="eyelash-l", left=2, top=1)
        psd.save(self.psd_path)
        mapping = []
        for index, layer in enumerate(PSDImage.open(self.psd_path)):
            crop = layer.topil().convert("RGBA")
            name = layer.name
            filename = name.replace(" ", "-") + ".png"
            entry = {"source_layer_id": f"{name}#{index}", "source_name": name,
                     "manifest_key": name, "recommended_manifest_value": filename,
                     "PNG_path": str(self.source / filename), "PSD_bbox": list(layer.bbox),
                     "expected_canvas": list(size), "RGBA_crop_sha256": pack.sha(crop.tobytes()),
                     "existing_PNG": name == "eyelash-l"}
            if entry["existing_PNG"]:
                pack.source_canvas(crop, layer.bbox, size).save(entry["PNG_path"])
                entry["existing_PNG_sha256"] = pack.file_sha(Path(entry["PNG_path"]))
            mapping.append(entry)
        self.audit = {"fixed_input_hashes": {"source.psd": pack.file_sha(self.psd_path)},
                      "PSD_vs_original_HEAD": {"canvas": list(size), "PSD_layer_count": 2},
                      "layer_manifest_audit": {"source_layer_mapping": mapping,
                          "proposed_manifest": {m["manifest_key"]: m["recommended_manifest_value"] for m in mapping}}}
        self.write_audit()

    def write_audit(self):
        self.audit_path.write_text(json.dumps(self.audit), encoding="utf8")

    def test_preserves_low_alpha_and_existing_png_bytes_without_changing_source(self):
        before = {p: p.read_bytes() for p in self.source.iterdir()}
        receipt = pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertEqual(receipt["copied_PNG_count"], 1)
        self.assertEqual(receipt["extracted_PSD_layer_count"], 1)
        with Image.open(self.output / "bite-mouth.png") as mouth:
            self.assertEqual(mouth.getpixel((5, 5)), (14, 72, 121, 2))
            self.assertEqual(mouth.getpixel((6, 4)), (219, 190, 145, 255))
            self.assertEqual(mouth.getpixel((4, 4)), (0, 0, 0, 0))
        for path, content in before.items():
            self.assertEqual(path.read_bytes(), content)
        self.assertEqual((self.output / "eyelash-l.png").read_bytes(), (self.source / "eyelash-l.png").read_bytes())

    def test_refuses_existing_output_and_source_overlap(self):
        self.output.mkdir()
        sentinel = self.output / "keep"
        sentinel.write_bytes(b"owned")
        with self.assertRaisesRegex(ValueError, "new directory"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertEqual(sentinel.read_bytes(), b"owned")
        with self.assertRaisesRegex(ValueError, "overlaps source"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.source / "new-pack")
        self.assertFalse((self.source / "new-pack").exists())

    def test_rejects_escaping_filename_before_any_output(self):
        mapping = self.audit["layer_manifest_audit"]
        mapping["source_layer_mapping"][0]["recommended_manifest_value"] = "../escaped.png"
        mapping["proposed_manifest"]["bite mouth"] = "../escaped.png"
        self.write_audit()
        with self.assertRaisesRegex(ValueError, "plain PNG"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertFalse(self.output.exists())

    def test_detects_psd_or_png_drift_before_any_output(self):
        existing = self.source / "eyelash-l.png"
        existing.write_bytes(existing.read_bytes() + b"drift")
        with self.assertRaisesRegex(ValueError, "PNG hash changed"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertFalse(self.output.exists())
        self.psd_path.write_bytes(self.psd_path.read_bytes() + b"drift")
        with self.assertRaisesRegex(ValueError, "PSD hash changed"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertFalse(self.output.exists())

    def test_detects_full_canvas_png_drift_even_when_crop_matches(self):
        existing = self.source / "eyelash-l.png"
        with Image.open(existing) as image:
            modified = image.convert("RGBA")
        modified.putpixel((14, 10), (15, 25, 35, 0))
        modified.save(existing)
        entry = next(m for m in self.audit["layer_manifest_audit"]["source_layer_mapping"] if m["existing_PNG"])
        entry["existing_PNG_sha256"] = pack.file_sha(existing)
        self.write_audit()
        with self.assertRaisesRegex(ValueError, "Full source PNG RGBA differs"):
            pack.prepare_pack(self.audit_path, self.psd_path, self.output)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
