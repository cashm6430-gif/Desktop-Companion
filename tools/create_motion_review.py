"""Create a review sheet from actual Cubism-rendered poses, never generated art.

Run build/DesktopCompanion.exe --review-motion build/motion-review first.
For an animated review, also run --render-motion build/motion-sequence.
For eye sweeps, run this script with --eye-manifest, then pass the returned
manifest to DesktopCompanion.exe --review-motion <eye-directory> <manifest>.
"""
import json
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    motion = json.loads((ROOT / "assets/motions/grass.motion.json").read_text(encoding="utf8"))
    revision = motion.get("revision", 1)
    eye_folder = ROOT / f"build/eye-v{revision}-review"
    eye_levels = ("1", "0.75", "0.5", "0.25", "0")
    if "--eye-manifest" in sys.argv:
        eye_folder.mkdir(parents=True, exist_ok=True)
        keys = [{"time": row * 5 + col, "label": f"{mode}-{level}", "parameters": {
            "ParamEyeLOpen": float(level), "ParamEyeROpen": float(level), "ParamEyeSmile": row,
            "ParamAngleX": 0, "ParamAngleY": 0, "ParamAngleZ": 0,
        }} for row, mode in enumerate(("open", "smile")) for col, level in enumerate(eye_levels)]
        manifest = eye_folder / "poses.motion.json"
        manifest.write_text(json.dumps({"keyframes": keys}, ensure_ascii=False, indent=2), encoding="utf8")
        print(manifest)
        return
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 24)
    small = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 18)
    poses = tuple(next(i for i, frame in enumerate(motion["keyframes"]) if frame["time"] == time)
                  for time in (0.8, 2.0, 3.6, 5.2))
    sheet = Image.new("RGB", (1120, 1204), "#e9edf5")
    draw = ImageDraw.Draw(sheet)
    draw.text((28, 18), f"狗尾巴草 · Live2D 草案 v{revision} · 连续眼睑 / 柔性草", font=font, fill="#1e2c47")
    draw.text((28, 53), "实际 Cubism 模型渲染 / 待审批 / 四个动作关键姿势", font=small, fill="#52617b")
    for cell, key in enumerate(poses):
        x, y = (cell % 2) * 560, 90 + (cell // 2) * 550
        draw.rounded_rectangle((x + 12, y + 8, x + 548, y + 538), radius=18, fill="#f8faff")
        sprite = Image.open(ROOT / f"build/motion-review/grass-{key:02}.png").convert("RGBA")
        sprite.thumbnail((488, 488), Image.Resampling.LANCZOS)
        sheet.paste(sprite, (x + (560 - sprite.width) // 2, y + 8), sprite)
        frame = motion["keyframes"][key]
        draw.text((x + 26, y + 495), f"{cell + 1}. {frame['label']}   {frame['time']:.1f}s", font=font, fill="#243654")
    output = ROOT / f"art/live2d/review/grass-keyframes-v{revision}.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output)
    print(output)
    detail = Image.new("RGB", (1180, 830), "#f8faff")
    detail_draw = ImageDraw.Draw(detail)
    detail_draw.text((24, 16), "表情近景 · 实际 Cubism 渲染 · 待审批", font=font, fill="#243654")
    for cell, key in enumerate(poses):
        x, y = 24 + cell % 2 * 590, 66 + cell // 2 * 380
        sprite = Image.open(ROOT / f"build/motion-review/grass-{key:02}.png").convert("RGBA")
        sprite = sprite.crop((290, 235, 570, 405)).resize((560, 340), Image.Resampling.LANCZOS)
        detail.paste(sprite, (x, y), sprite)
        detail_draw.text((x, y + 342), motion["keyframes"][key]["label"], font=small, fill="#52617b")
    detail_output = output.with_name(f"grass-expression-detail-v{revision}.png")
    detail.save(detail_output)
    print(detail_output)

    if all((eye_folder / f"grass-{index:02}.png").is_file() for index in range(10)):
        eyes = Image.new("RGB", (1400, 510), "#f8faff")
        eye_draw = ImageDraw.Draw(eyes)
        for row, (mode, label) in enumerate((("open", "普通眨眼"), ("smile", "笑眼"))):
            y = row * 255
            eye_draw.text((12, y + 4), label + " · 眼睑形变 / 虹膜遮罩 · Native 渲染", font=small, fill="#243654")
            for col, level in enumerate(eye_levels):
                eye = Image.open(eye_folder / f"grass-{row * 5 + col:02}.png").convert("RGBA").crop((290, 235, 570, 405))
                eyes.paste(eye, (col * 280, y + 36), eye)
                eye_draw.text((col * 280 + 12, y + 212), f"开合 {level}", font=small, fill="#52617b")
        eyes.save(output.with_name(f"eye-closure-v{revision}.png"))

    sequence = sorted((ROOT / "build/motion-sequence").glob("frame-*.png"))
    if len(sequence) == 120:
        # This GIF is a review artifact made from actual Native frames. The
        # desktop pet continues to animate the MOC3, and never plays this file.
        frames = []
        for path in sequence:
            image = Image.open(path).convert("RGBA").resize((420, 420), Image.Resampling.LANCZOS)
            matte = Image.new("RGB", image.size, "#f8faff")
            matte.paste(image, (0, 0), image)
            frames.append(matte)
        palette_source = Image.new("RGB", (420 * 4, 420))
        for i, frame in enumerate((frames[12], frames[30], frames[54], frames[78])):
            palette_source.paste(frame, (420 * i, 0))
        palette = palette_source.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
        frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
        frames[0].save(output.with_name(f"grass-motion-v{revision}.gif"), save_all=True, append_images=frames[1:],
                       duration=[70, 70, 60] * 40, loop=0, optimize=False, disposal=2)
        print(output.with_name(f"grass-motion-v{revision}.gif"))


if __name__ == "__main__":
    main()
