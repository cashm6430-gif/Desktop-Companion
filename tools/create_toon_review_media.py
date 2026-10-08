"""Build diagnostic media from actual Godot GPU frames, never authored stills."""

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


BACKGROUND = (239, 243, 249, 255)
KEYS = ("standing", "blink", "wave", "half-sit", "seated", "rise", "turn45", "idle-return")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matte(path):
    with Image.open(path) as source:
        rgba = source.convert("RGBA")
        return Image.alpha_composite(Image.new("RGBA", rgba.size, BACKGROUND), rgba).convert("RGB")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--review", type=Path, required=True)
    args = parser.parse_args()
    review = args.review.resolve()
    captures = review / "captures"
    manifest = json.loads((captures / "sequence-manifest.json").read_text(encoding="utf8"))
    records = manifest["frames"][:240]
    if len(records) != 240 or any(item.get("source") != "gpu_viewport_after_frame_post_draw" for item in records):
        parser.error("Expected a full 16-second, 15 fps sequence from the real GPU viewport")
    # GIF timings are in centiseconds. Distribute rounding instead of speeding
    # a 15 fps sequence up by assigning every frame an identical 60 ms duration.
    frames = [matte(captures / item["file"]) for item in records]
    durations = [10 * (round((i + 1) * 100 / 15) - round(i * 100 / 15)) for i in range(len(frames))]
    frames[0].save(review / "motion-preview.gif", save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, optimize=False)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    font = ImageFont.truetype(str(font_path), 14) if font_path.exists() else ImageFont.load_default()
    sheet = Image.new("RGB", (960, 608), BACKGROUND[:3])
    draw = ImageDraw.Draw(sheet)
    for index, name in enumerate(KEYS):
        image = matte(captures / (name + ".png")).resize((240, 280), Image.Resampling.LANCZOS)
        x, y = (index % 4) * 240, (index // 4) * 304
        sheet.paste(image, (x, y))
        draw.text((x + 12, y + 282), name, fill=(35, 49, 78), font=font)
    sheet.save(review / "keyposes-sheet.png")
    sheet = Image.new("RGB", (960, 960), BACKGROUND[:3])
    draw = ImageDraw.Draw(sheet)
    for index in range(48):
        item = records[index * 5]
        image = matte(captures / item["file"]).resize((120, 140), Image.Resampling.LANCZOS)
        x, y = (index % 8) * 120, (index // 8) * 160
        sheet.paste(image, (x, y))
        draw.text((x + 3, y + 142), f"{item['animation_time']:.2f} {item['pose']['state']}",
                  fill=(35, 49, 78), font=font)
    sheet.save(review / "continuity-sheet.png")
    receipt = {"kind": "gpu_capture_diagnostic_media", "source": "actual_GPU_sequence_frames",
               "sequence_manifest_sha256": digest(captures / "sequence-manifest.json"),
               "frames_used": 240, "duration_seconds": 16, "background_matte": list(BACKGROUND),
               "generator_sha256": digest(Path(__file__)),
               "outputs": {name: digest(review / name) for name in
                           ("motion-preview.gif", "keyposes-sheet.png", "continuity-sheet.png")},
               "art_approval": "not_requested_technical_placeholder"}
    (review / "media-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf8")
    print(json.dumps({"frames": 240, "seconds": 16, "output": str(review)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
