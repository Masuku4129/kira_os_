#!/usr/bin/env python3
"""JPEG для проверки просмотра: Pillow (libjpeg) кодирует, эталон - его же раскодированные пиксели (.rgb: ширина,
высота по 4 байта, затем RGB). Прогрессивные (SOF2: спектральная выборка, последовательное уточнение, перезапуск) - те же пиксели, что у libjpeg;
big-prog.jpg - прогрессивный 4:4:4, у которого буфер коэффициентов больше предела программы: честный отказ."""
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
                           ("gray.jpg", 41, 27, dict(quality=80)),
                           ("prog.jpg", 97, 61, dict(quality=90, subsampling=2, progressive=True)),          # 4:2:0
                           ("prog422.jpg", 64, 40, dict(quality=85, subsampling=1, progressive=True)),
                           ("prog444.jpg", 50, 33, dict(quality=95, subsampling=0, progressive=True)),
                           ("prog-rst.jpg", 83, 70, dict(quality=88, subsampling=2, progressive=True, restart_marker_blocks=2)),
                           ("prog-gray.jpg", 41, 27, dict(quality=80, progressive=True)),
                           ("prog-opt.jpg", 120, 90, dict(quality=60, subsampling=0, progressive=True, optimize=True))]:
        im = picture(w, h)
        if name in ("gray.jpg", "prog-gray.jpg"): im = im.convert("L")
        p = os.path.join(out, name); im.save(p, "JPEG", **kw); files["/" + name] = p
        ref = Image.open(p).convert("RGB")
        open(p + ".rgb", "wb").write(struct.pack("<II", w, h) + ref.tobytes())
    p = os.path.join(out, "big-prog.jpg"); picture(840, 840).save(p, "JPEG", quality=30, subsampling=0, progressive=True); files["/big-prog.jpg"] = p
    return files

if __name__ == "__main__":
    print(make(sys.argv[1]))
