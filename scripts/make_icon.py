"""Regenerates res/MiniCCServe.ico (run: uv run --with pillow scripts/make_icon.py).

Design: dark rounded square, white play triangle ("start serving"), green
status LED ("server running"). Drawn large and downsampled into a multi-size
.ico so small sizes stay crisp.
"""

from pathlib import Path

from PIL import Image, ImageDraw

RES = Path(__file__).resolve().parent.parent / "res"
SIZE = 1024


def draw_master(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = size / SIZE  # scale factor

    def box(x0, y0, x1, y1):
        return [x0 * s, y0 * s, x1 * s, y1 * s]

    # Dark slate rounded square
    d.rounded_rectangle(box(24, 24, SIZE - 24, SIZE - 24), radius=210 * s, fill=(15, 23, 42, 255))
    # Subtle top edge highlight
    d.rounded_rectangle(box(24, 24, SIZE - 24, 24 + 26), radius=13 * s, fill=(51, 65, 85, 255))

    # White play triangle
    d.polygon([(368 * s, 316 * s), (368 * s, 708 * s), (694 * s, 512 * s)], fill=(226, 232, 240, 255))

    # Green status LED, bottom right
    led_r = 62 * s
    cx, cy = 772 * s, 772 * s
    d.ellipse([cx - led_r, cy - led_r, cx + led_r, cy + led_r], fill=(52, 211, 153, 255))
    d.ellipse([cx - 34 * s, cy - 34 * s, cx + 34 * s, cy + 34 * s], fill=(167, 243, 208, 255))
    return img


def main() -> None:
    master = draw_master(SIZE)
    RES.mkdir(exist_ok=True)
    out = RES / "MiniCCServe.ico"
    master.save(
        out,
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
