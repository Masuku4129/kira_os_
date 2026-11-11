#!/usr/bin/env python3
"""Generate the deterministic 97x61 JPEG fixture used by native and ARM64 viewer tests."""
import os
from PIL import Image


def make(out):
    width, height = 97, 61
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels.extend((x * 255 // (width - 1),
                           y * 255 // (height - 1),
                           (x * y) & 255))
    image = Image.frombytes("RGB", (width, height), bytes(pixels))
    path = os.path.join(out, "pic.jpg")
    image.save(path, format="JPEG", quality=90, subsampling=2,
               optimize=False, progressive=False)
    return {"/pic.jpg": path}
