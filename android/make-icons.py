#!/usr/bin/env python3
"""Generate Android launcher icons from a square source image.

Two separate icon systems, with different canvas sizes - getting these confused is
why an icon looks soft:

  legacy (pre-API 26)   48dp canvas, full-bleed. 48/72/96/144/192 px.
  adaptive (API 26+)    108dp canvas, of which only the inner ~72dp survives the
                        launcher's mask. 108/162/216/324/432 px.

The adaptive foreground is the one modern launchers actually draw, so it must be
authored at 108dp - not at the legacy 48dp sizes, or the launcher upscales it.
An xxhdpi device (which includes the Ayn Thor at 369dpi) wants a 324px foreground.

The launcher masks to its own shape - circle, squircle, rounded square - and crops
about 25% off each edge, so the artwork is inset into the safe zone and sits on a
solid background sampled from the art.

Requires Pillow. Usage:
    python3 android/make-icons.py ~/Downloads/selaco-keyart.png
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required:  python3 -m pip install pillow")

# name -> (legacy px, adaptive px)
DENSITIES = {
    "mdpi":    (48, 108),
    "hdpi":    (72, 162),
    "xhdpi":   (96, 216),
    "xxhdpi":  (144, 324),
    "xxxhdpi": (192, 432),
}

# Fraction of the 108dp adaptive canvas the art occupies. The spec's safe zone is
# 72/108 = 0.667; staying just inside it keeps content clear of a circular mask.
SAFE_FRACTION = 0.64

PLAY_STORE_PX = 512


def load_square(path):
    src = Image.open(path).convert("RGBA")
    if src.width != src.height:
        side = min(src.size)
        left = (src.width - side) // 2
        top = (src.height - side) // 2
        src = src.crop((left, top, left + side, top + side))
    return src


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = load_square(os.path.expanduser(sys.argv[1]))
    here = os.path.dirname(os.path.abspath(__file__))
    res = os.path.join(here, "app", "src", "main", "res")

    for name, (legacy_px, adaptive_px) in DENSITIES.items():
        outdir = os.path.join(res, f"mipmap-{name}")
        os.makedirs(outdir, exist_ok=True)

        # Legacy: full-bleed square.
        src.resize((legacy_px, legacy_px), Image.LANCZOS).save(
            os.path.join(outdir, "ic_launcher.png"))

        # Adaptive foreground: transparent 108dp canvas, art inset to the safe zone.
        fg = Image.new("RGBA", (adaptive_px, adaptive_px), (0, 0, 0, 0))
        inner = int(adaptive_px * SAFE_FRACTION)
        art = src.resize((inner, inner), Image.LANCZOS)
        offset = (adaptive_px - inner) // 2
        fg.paste(art, (offset, offset), art)
        fg.save(os.path.join(outdir, "ic_launcher_foreground.png"))

        print(f"  mipmap-{name:8} legacy {legacy_px:3}px   foreground {adaptive_px:3}px")

    # Not used by the launcher, but the Play Store listing wants 512x512.
    src.resize((PLAY_STORE_PX, PLAY_STORE_PX), Image.LANCZOS).save(
        os.path.join(here, "app", "src", "main", "ic_launcher-playstore.png"))
    print(f"  ic_launcher-playstore.png {PLAY_STORE_PX}x{PLAY_STORE_PX}")


if __name__ == "__main__":
    main()
