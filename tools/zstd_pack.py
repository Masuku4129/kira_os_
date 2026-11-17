#!/usr/bin/env python3
"""Сжать файл в кадр Zstd (с размером содержимого и контрольной суммой) для `kfstool образ putz`. Нет python-zstandard - код 3 (вызывающий кладёт файл как есть)."""
import sys
try:
    import zstandard
except ImportError:
    sys.exit(3)
data = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(zstandard.ZstdCompressor(level=19, write_checksum=True, write_content_size=True).compress(data))
