#!/usr/bin/env python3
"""Draw the FaultCodes background (1120x352): a header bar and four code rows.
Usage: make_fault_art.py OUT.zip  -> zip containing 56800.png"""
import io, sys, zipfile
from PIL import Image, ImageDraw

W, H = 1120, 352
HEADER = (34, 44, 56, 255)
ROW = (23, 27, 34, 255)
ACCENT = (96, 146, 196, 255)
DIVIDER = (52, 58, 68, 255)

img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((0, 0, W - 1, 62), radius=12, fill=HEADER)
for i in range(4):
    top = 70 + i * 67
    d.rounded_rectangle((0, top, W - 1, top + 59), radius=10, fill=ROW)
    d.rounded_rectangle((8, top + 15, 12, top + 44), radius=2, fill=ACCENT)
    d.line((281, top + 10, 281, top + 49), fill=DIVIDER, width=2)
png = io.BytesIO()
img.save(png, "PNG", optimize=True)
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    info = zipfile.ZipInfo("56800.png", date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    z.writestr(info, png.getvalue())
