#!/usr/bin/env python3
"""Build macos/../src/posix/osx/selaco.icns from a square source image.

macOS icons are not full-bleed squares. Since Big Sur the convention is a rounded
rectangle inset within a transparent canvas, so icons share a silhouette in the
Dock and Finder. Apple's proportions, on a 1024pt canvas:

    body        824 x 824, centred (100pt margin on every side)
    corner      185.4pt radius  (~22.5% of the body)

A full-bleed square looks like a photo tile next to native apps, which is why the
source art gets masked rather than just scaled.

Requires Pillow. Usage:
    python3 macos/make-icon.py ~/Downloads/selaco-keyart.png
"""
import os
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required:  python3 -m pip install pillow")

CANVAS = 1024
BODY = 824
RADIUS = 185
SUPERSAMPLE = 4          # mask is drawn large and downsampled for clean edges
ICNS_SIZES = (16, 32, 128, 256, 512)


def rounded_icon(src_path):
    src = Image.open(src_path).convert("RGBA")
    if src.width != src.height:
        side = min(src.size)
        left = (src.width - side) // 2
        top = (src.height - side) // 2
        src = src.crop((left, top, left + side, top + side))

    art = src.resize((BODY, BODY), Image.LANCZOS)

    # Draw the mask at SUPERSAMPLE x and shrink it: PIL's rounded_rectangle is not
    # antialiased, so drawing it at final size leaves visibly jagged corners.
    big = BODY * SUPERSAMPLE
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, big - 1, big - 1), radius=RADIUS * SUPERSAMPLE, fill=255
    )
    mask = mask.resize((BODY, BODY), Image.LANCZOS)

    art.putalpha(mask)

    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    offset = (CANVAS - BODY) // 2
    canvas.paste(art, (offset, offset), art)
    return canvas


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    source = os.path.expanduser(sys.argv[1])
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, os.pardir, "src", "posix", "osx", "selaco.icns")

    master = rounded_icon(source)

    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "Selaco.iconset")
        os.mkdir(iconset)
        # iconutil requires exactly this naming, with a 1x and 2x for each size.
        for base in ICNS_SIZES:
            master.resize((base, base), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{base}x{base}.png"))
            master.resize((base * 2, base * 2), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{base}x{base}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)

    print(f"wrote {os.path.normpath(out)}  ({os.path.getsize(out) // 1024} KB)")


if __name__ == "__main__":
    main()
