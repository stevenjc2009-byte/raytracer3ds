import wave
import struct
import math
from PIL import Image, ImageDraw, ImageFont

STAGE = r"C:\Users\steve\AppData\Local\Temp\claude\C--Users-steve-Documents-fuck-u\9f131b7d-bfd9-4ba8-aff6-f75ae78d5bf4\scratchpad\rt3ds-cia"

def get_font(size):
    for path in (
        r"C:\Windows\Fonts\arialbd.ttf",
        r"C:\Windows\Fonts\arial.ttf",
    ):
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            continue
    return ImageFont.load_default()

def draw_placeholder_cube(draw, cx, cy, s, outline):
    # simple isometric-looking cube wireframe as a generic "rendered" placeholder shape
    top = [(cx, cy - s), (cx + s, cy - s * 0.5), (cx, cy), (cx - s, cy - s * 0.5)]
    draw.polygon(top, outline=outline, fill=(60, 60, 70))
    left = [(cx - s, cy - s * 0.5), (cx, cy), (cx, cy + s), (cx - s, cy + s * 0.5)]
    draw.polygon(left, outline=outline, fill=(40, 40, 48))
    right = [(cx + s, cy - s * 0.5), (cx, cy), (cx, cy + s), (cx + s, cy + s * 0.5)]
    draw.polygon(right, outline=outline, fill=(50, 50, 58))

# ---- Icon: 48x48 ----
icon = Image.new("RGB", (48, 48), (24, 26, 32))
d = ImageDraw.Draw(icon)
draw_placeholder_cube(d, 24, 26, 14, (90, 90, 100))
font = get_font(14)
d.text((10, 2), "RT", font=font, fill=(220, 220, 230))
icon.save(f"{STAGE}/icon48.png")

# ---- Small icon: 24x24 ----
icon_small = icon.resize((24, 24), Image.LANCZOS)
icon_small.save(f"{STAGE}/icon24.png")

# ---- Banner: 256x128 ----
banner = Image.new("RGB", (256, 128), (24, 26, 32))
d = ImageDraw.Draw(banner)
draw_placeholder_cube(d, 70, 64, 40, (90, 90, 100))
font_big = get_font(28)
font_small = get_font(12)
d.text((130, 40), "RAYTRACER3DS", font=font_big, fill=(220, 220, 230))
d.text((130, 78), "PLACEHOLDER ART", font=font_small, fill=(160, 160, 170))
banner.save(f"{STAGE}/banner256.png")

print("icon/banner PNGs written")

# ---- Minimal silent WAV for banner audio ----
# 16-bit mono PCM, 22050 Hz, ~0.5s of silence (bannertool needs a valid WAV, not empty)
sample_rate = 22050
duration_s = 0.5
n_samples = int(sample_rate * duration_s)
with wave.open(f"{STAGE}/banner_audio.wav", "w") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sample_rate)
    frames = struct.pack("<%dh" % n_samples, *([0] * n_samples))
    w.writeframes(frames)

print("silent placeholder WAV written")
