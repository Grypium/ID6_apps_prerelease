# Let Pillow find fonts by bare name on Termux (sys.platform == "android").
import os
try:
    from PIL import ImageFont
    _orig = ImageFont.truetype
    _dir = os.path.join(os.environ.get("PREFIX", "/data/data/com.termux/files/usr"), "share/fonts/TTF")
    def truetype(font=None, *a, **k):
        if isinstance(font, str) and not os.path.exists(font) and os.path.exists(os.path.join(_dir, font)):
            font = os.path.join(_dir, font)
        return _orig(font, *a, **k)
    ImageFont.truetype = truetype
except Exception:
    pass
