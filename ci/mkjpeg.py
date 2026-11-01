#!/usr/bin/env python3
"""JPEG для проверки просмотра: Pillow (libjpeg) кодирует, эталон - его же раскодированные пиксели (.rgb: ширина,
высота по 4 байта, затем RGB). Прогрессивный - чтобы проверить честный отказ."""
import os, struct, sys
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfiles import img

def make(out):
    os.makedirs(out, exist_ok=True)
    def picture(w, h):
        im = Image.new("RGB", (w, h)); px = img(w, h)
        im.putdata([(c >> 16, (c >> 8) & 255, c & 255) for c in px]); return im
    files = {}
    for name, w, h, kw in [("pic.jpg", 97, 61, dict(quality=90, subsampling=2)),           # 4:2:0
                           ("p422.jpg", 64, 40, dict(quality=85, subsampling=1)),
                           ("p444.jpg", 50, 33, dict(quality=95, subsampling=0, restart_marker_blocks=1)),
                           ("gray.jpg", 41, 27, dict(quality=80))]:
        im = picture(w, h)
        if name == "gray.jpg": im = im.convert("L")
        p = os.path.join(out, name); im.save(p, "JPEG", **kw); files["/" + name] = p
        ref = Image.open(p).convert("RGB")
        open(p + ".rgb", "wb").write(struct.pack("<II", w, h) + ref.tobytes())
    p = os.path.join(out, "prog.jpg"); picture(30, 20).save(p, "JPEG", progressive=True); files["/prog.jpg"] = p
    return files

if __name__ == "__main__":
    print(make(sys.argv[1]))
