"""Compose diagnostic sheets/preview from unmodified real GPU captures."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matte(path, size):
    with Image.open(path) as source:
        image = source.convert("RGBA")
        image.thumbnail(size, Image.Resampling.LANCZOS)
        background = Image.new("RGBA", size, (235, 241, 250, 255))
        background.alpha_composite(image, ((size[0] - image.width) // 2, (size[1] - image.height) // 2))
        return background.convert("RGB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", required=True, type=Path)
    args = parser.parse_args()
    review = args.review.resolve()
    captures = review / "captures"
    report = json.loads((captures / "report.json").read_text(encoding="utf8"))
    if report.get("headless") is not False or not report.get("visual_capture_completed"):
        parser.error("Only complete real GPU reviews are supported")
    keys = report["captures"]
    font_file = Path("C:/Windows/Fonts/segoeui.ttf")
    font = ImageFont.truetype(str(font_file), 12) if font_file.is_file() else ImageFont.load_default()
    outputs = []
    for width, height, label in ((300, 350, "contact-sheet"), (240, 280, "desktop-size-sheet")):
        columns, rows = min(3, len(keys)), (len(keys) + 2) // 3
        sheet = Image.new("RGB", (columns * width, rows * (height + 38)), (235, 241, 250))
        draw = ImageDraw.Draw(sheet)
        for index, item in enumerate(keys):
            path = captures / item["file"]
            pose = item["pose"]
            x, y = (index % columns) * width, (index // columns) * (height + 38)
            sheet.paste(matte(path, (width, height)), (x, y))
            draw.text((x + 8, y + height + 2), Path(item["file"]).stem, fill=(35, 49, 78), font=font)
            draw.text((x + 8, y + height + 18), f"{item['actual_time']:.2f}s  {pose['state']}", fill=(35, 49, 78), font=font)
        filename = label + ".png"
        sheet.save(review / filename)
        outputs.append(filename)
    sequence = captures / "sequence-manifest.json"
    records = []
    if sequence.is_file():
        manifest = json.loads(sequence.read_text(encoding="utf8"))
        records = manifest["frames"]
        if len(records) < 2:
            parser.error("Sequence does not contain enough frames")
        for item in records:
            if item.get("source") != "gpu_viewport_after_frame_post_draw" or digest(captures / item["file"]) != item["sha256"]:
                parser.error("Sequence provenance or hash mismatch")
        if any(right["animation_time"] <= left["animation_time"] for left, right in zip(records, records[1:])):
            parser.error("Sequence clock is not strictly increasing")
        times = [item["animation_time"] for item in records]
        origin = times[0]
        rounded = [round((t - origin) * 100) for t in times]
        durations = [max(1, b - a) * 10 for a, b in zip(rounded, rounded[1:])]
        durations.append(durations[-1])
        frames = [matte(captures / item["file"], (360, 420)) for item in records]
        frames[0].save(review / "motion-preview.gif", save_all=True, append_images=frames[1:],
                       duration=durations, loop=0, optimize=False)
        outputs.append("motion-preview.gif")
    receipt = {"schema_version": 1, "source": "actual_GPU_viewport_captures",
               "report_sha256": digest(captures / "report.json"), "generator_sha256": digest(Path(__file__)),
               "key_images": {item["file"]: digest(captures / item["file"]) for item in keys},
               "sequence_manifest_sha256": digest(sequence) if sequence.is_file() else None,
               "sequence_frames": len(records), "clock": report.get("clock"),
               "image_processing": "uniform_resize_and_diagnostic_background_matte_only",
               "visual_approval": "pending", "outputs": {name: digest(review / name) for name in outputs}}
    (review / "media-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"output": str(review), "files": outputs, "sequence_frames": len(records)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
