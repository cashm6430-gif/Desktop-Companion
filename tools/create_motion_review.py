"""Create a review sheet from actual Cubism-rendered poses, never generated art.

Run build/DesktopCompanion.exe --review-motion build/motion-review first.
"""
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    motion = json.loads((ROOT / "assets/motions/grass.motion.json").read_text(encoding="utf8"))
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 24)
    small = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 18)
    sheet = Image.new("RGB", (1120, 1204), "#e9edf5")
    draw = ImageDraw.Draw(sheet)
    draw.text((28, 18), "狗尾巴草 · Live2D 关键姿势草案 v2 · 掌心握持", font=font, fill="#1e2c47")
    draw.text((28, 53), "实际 Cubism 模型渲染 / 待审批 / 四个动作关键姿势", font=small, fill="#52617b")
    for cell, key in enumerate((1, 2, 3, 4)):
        x, y = (cell % 2) * 560, 90 + (cell // 2) * 550
        draw.rounded_rectangle((x + 12, y + 8, x + 548, y + 538), radius=18, fill="#f8faff")
        sprite = Image.open(ROOT / f"build/motion-review/grass-{key:02}.png").convert("RGBA")
        sprite.thumbnail((488, 488), Image.Resampling.LANCZOS)
        sheet.paste(sprite, (x + (560 - sprite.width) // 2, y + 8), sprite)
        frame = motion["keyframes"][key]
        draw.text((x + 26, y + 495), f"{cell + 1}. {frame['label']}   {frame['time']:.1f}s", font=font, fill="#243654")
    output = ROOT / "art/live2d/review/grass-keyframes-v2.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output)
    print(output)
    detail = Image.new("RGB", (920, 420), "#f8faff")
    detail_draw = ImageDraw.Draw(detail)
    detail_draw.text((24, 16), "掌心握持细节 · 实际 Cubism 渲染", font=font, fill="#243654")
    for cell, (key, box) in enumerate(((1, (580, 486, 680, 566)), (3, (538, 464, 638, 544)))):
        sprite = Image.open(ROOT / f"build/motion-review/grass-{key:02}.png").convert("RGBA")
        sprite = sprite.crop(box).resize((400, 320), Image.Resampling.LANCZOS)
        detail.paste(sprite, (30 + cell * 460, 62), sprite)
        detail_draw.text((30 + cell * 460, 387), motion["keyframes"][key]["label"], font=small, fill="#52617b")
    detail_output = output.with_name("grass-grip-detail-v2.png")
    detail.save(detail_output)
    print(detail_output)


if __name__ == "__main__":
    main()
