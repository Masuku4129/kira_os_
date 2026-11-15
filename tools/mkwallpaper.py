#!/usr/bin/env python3
"""Любая картинка -> KWP1 (обои Kira OS): mkwallpaper.py вход.(png|jpg|...) выход.kwp [ширина высота]
По умолчанию 1280x720 с обрезкой до 16:9. Нужен Pillow."""
import struct, sys
from PIL import Image, ImageOps
src, dst = sys.argv[1], sys.argv[2]
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (1280, 720)
im = ImageOps.fit(Image.open(src).convert("RGB"), (w, h), Image.LANCZOS)
open(dst, "wb").write(b"KWP1" + struct.pack("<II", w, h) + im.tobytes())
print(f"{dst}: {w}x{h}, {w*h*3+12} байт")
