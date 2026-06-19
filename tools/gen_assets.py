#!/usr/bin/env python3
"""Generate mvedit's art assets.

Outputs (all under assets/):
  - app-icon.png          24x24, 4-color program icon (for png-to-mvicon)
  - default-palette.txt   the 4-color icon palette
  - app-palette.txt       1bpp image palette (idx0=black, idx1=white)

mvedit has no toolbar buttons (it is a text editor), so unlike mvdraw this
generator only writes the launcher icon and the two palettes.

Run from the project root:  python3 tools/gen_assets.py
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
os.makedirs(ASSETS, exist_ok=True)

N = 24                      # icon size

# CoCo color component 0..3 -> 8-bit sample.
C = (0, 85, 170, 255)
def coco(r, g, b):
    return (C[r], C[g], C[b])


def write_palette(path, entries):
    with open(path, "w") as f:
        for i, (r, g, b) in enumerate(entries):
            f.write("PALET%d=%d,%d,%d\n" % (i, r, g, b))

# 1bpp image palette: only indices 0 (black) and 1 (white) are used at 1bpp.
app_pal = [(0, 0, 0), (3, 3, 3)] + [(0, 0, 0)] * 14
write_palette(os.path.join(ASSETS, "app-palette.txt"), app_pal)

# Icon palette (4 meaningful colors; rest are filler):
#   0 white paper, 1 black ink, 2 dark grey, 3 cyan-blue accent.
icon_pal = [(3, 3, 3), (0, 0, 0), (1, 1, 1), (0, 2, 3)] + [(0, 0, 0)] * 12
write_palette(os.path.join(ASSETS, "default-palette.txt"), icon_pal)

ICON_WHITE = coco(3, 3, 3)
ICON_BLACK = coco(0, 0, 0)
ICON_GREY = coco(1, 1, 1)
ICON_BLUE = coco(0, 2, 3)


def app_icon():
    """A sheet of paper with a turned-down corner and lines of text -- a
    document/editor motif."""
    img = Image.new("RGB", (N, N), ICON_WHITE)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, N - 1, N - 1], outline=ICON_BLACK)        # frame

    # page body with a dog-eared top-right corner
    page = [(4, 2), (16, 2), (20, 6), (20, 21), (4, 21)]
    d.polygon(page, fill=ICON_WHITE, outline=ICON_BLACK)
    d.polygon([(16, 2), (20, 6), (16, 6)], fill=ICON_GREY, outline=ICON_BLACK)

    # lines of "text"
    for y in range(8, 20, 3):
        d.line([6, y, 18 if y < 17 else 13, y], fill=ICON_BLACK)

    # a blue text caret on the first line
    d.line([6, 7, 6, 9], fill=ICON_BLUE)
    img.save(os.path.join(ASSETS, "app-icon.png"))


def main():
    app_icon()
    im = Image.open(os.path.join(ASSETS, "app-icon.png"))
    assert im.size == (N, N), "app-icon is %s" % (im.size,)
    print("wrote assets/app-icon.png", im.size,
          "+ default-palette.txt + app-palette.txt")


if __name__ == "__main__":
    main()
