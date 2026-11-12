#!/usr/bin/env python3
"""Reproducible, dependency-free data-layout benchmark for KIRA Technology Lab.

This is a host-side format experiment, not a claim about KiraFS or kernel speed.
It checks round-trip correctness before reporting size and encode/decode timings.
"""
import argparse
import csv
import json
import os
import statistics
import struct
import time
import zlib

REC = struct.Struct("<IQiBB")
U32 = struct.Struct("<I")
U64 = struct.Struct("<Q")
I32 = struct.Struct("<i")


def make_rows(count):
    rows = []
    for i in range(count):
        # Deterministic mixed workload: monotonic IDs/time, signed values,
        # low-cardinality category, and flags.
        rows.append((i, 1_700_000_000_000 + i * 7,
                     ((i * 7919) % 200_001) - 100_000,
                     (i // 13 + i // 997) % 16,
                     (i * 5) % 4))
    return rows


def put_varint(out, value):
    if value < 0:
        raise ValueError("varint requires an unsigned value")
    while value >= 0x80:
        out.append((value & 0x7f) | 0x80)
        value >>= 7
    out.append(value)


def get_varint(buf, pos):
    value = shift = 0
    while True:
        if pos >= len(buf) or shift > 63:
            raise ValueError("invalid/truncated varint")
        byte = buf[pos]
        pos += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def enc_json(rows):
    return json.dumps(rows, separators=(",", ":"), ensure_ascii=False).encode()


def dec_json(blob):
    return [tuple(row) for row in json.loads(blob)]


def enc_fixed(rows):
    out = bytearray(U32.pack(len(rows)))
    for row in rows:
        out.extend(REC.pack(*row))
    return bytes(out)


def dec_fixed(blob):
    if len(blob) < 4:
        raise ValueError("short fixed header")
    count = U32.unpack_from(blob)[0]
    if len(blob) != 4 + count * REC.size:
        raise ValueError("fixed record length mismatch")
    return [tuple(row) for row in REC.iter_unpack(blob[4:])]


def enc_columnar(rows):
    n = len(rows)
    cols = [bytearray(), bytearray(), bytearray(), bytearray(), bytearray()]
    for ident, stamp, value, category, flags in rows:
        cols[0].extend(U32.pack(ident))
        cols[1].extend(U64.pack(stamp))
        cols[2].extend(I32.pack(value))
        cols[3].append(category)
        cols[4].append(flags)
    return U32.pack(n) + b"".join(cols)


def dec_columnar(blob):
    if len(blob) < 4:
        raise ValueError("short columnar header")
    n = U32.unpack_from(blob)[0]
    expected = 4 + n * (4 + 8 + 4 + 1 + 1)
    if len(blob) != expected:
        raise ValueError("columnar length mismatch")
    p = 4
    ids = [x[0] for x in struct.iter_unpack("<I", blob[p:p+n*4])]
    p += n * 4
    stamps = [x[0] for x in struct.iter_unpack("<Q", blob[p:p+n*8])]
    p += n * 8
    values = [x[0] for x in struct.iter_unpack("<i", blob[p:p+n*4])]
    p += n * 4
    cats = blob[p:p+n]
    p += n
    flags = blob[p:p+n]
    return list(zip(ids, stamps, values, cats, flags))


def enc_delta_varint(rows):
    out = bytearray(U32.pack(len(rows)))
    prev_id = prev_stamp = 0
    for ident, stamp, value, category, flags in rows:
        put_varint(out, ident - prev_id)
        put_varint(out, stamp - prev_stamp)
        zigzag = (value << 1) ^ (value >> 31)
        put_varint(out, zigzag)
        out.append(category)
        out.append(flags)
        prev_id, prev_stamp = ident, stamp
    return bytes(out)


def dec_delta_varint(blob):
    if len(blob) < 4:
        raise ValueError("short varint header")
    count = U32.unpack_from(blob)[0]
    pos = 4
    ident = stamp = 0
    rows = []
    for _ in range(count):
        d_id, pos = get_varint(blob, pos)
        d_stamp, pos = get_varint(blob, pos)
        zz, pos = get_varint(blob, pos)
        if pos + 2 > len(blob):
            raise ValueError("truncated varint record")
        ident += d_id
        stamp += d_stamp
        value = (zz >> 1) ^ -(zz & 1)
        rows.append((ident, stamp, value, blob[pos], blob[pos+1]))
        pos += 2
    if pos != len(blob):
        raise ValueError("trailing varint bytes")
    return rows


def enc_json_zlib(rows):
    return zlib.compress(enc_json(rows), level=6)


def dec_json_zlib(blob):
    return dec_json(zlib.decompress(blob))


def enc_columnar_zlib(rows):
    return zlib.compress(enc_columnar(rows), level=6)


def dec_columnar_zlib(blob):
    return dec_columnar(zlib.decompress(blob))


FORMATS = [
    ("json_compact", enc_json, dec_json),
    ("fixed_binary", enc_fixed, dec_fixed),
    ("columnar_soa", enc_columnar, dec_columnar),
    ("delta_varint_tlv", enc_delta_varint, dec_delta_varint),
    ("json_zlib_6", enc_json_zlib, dec_json_zlib),
    ("columnar_zlib_6", enc_columnar_zlib, dec_columnar_zlib),
]


def median_ms(fn, repeat):
    samples = []
    result = None
    for _ in range(repeat):
        start = time.perf_counter()
        result = fn()
        samples.append((time.perf_counter() - start) * 1000.0)
    return result, statistics.median(samples)


def run_case(rows, repeat):
    results = []
    expected = rows
    for name, encode, decode in FORMATS:
        blob, enc_ms = median_ms(lambda: encode(rows), repeat)
        decoded, dec_ms = median_ms(lambda: decode(blob), repeat)
        if decoded != expected:
            raise AssertionError(f"{name}: round-trip mismatch")
        results.append({
            "format": name,
            "rows": len(rows),
            "bytes": len(blob),
            "bytes_per_row": len(blob) / max(1, len(rows)),
            "encode_ms": round(enc_ms, 3),
            "decode_ms": round(dec_ms, 3),
            "encode_rows_s": round(len(rows) / (enc_ms / 1000), 1) if enc_ms else 0,
            "decode_rows_s": round(len(rows) / (dec_ms / 1000), 1) if dec_ms else 0,
            "roundtrip": "PASS",
        })
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=50000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--out", default="ci-out/data-format-lab")
    args = ap.parse_args()
    if args.rows < 1 or args.repeat < 1:
        ap.error("--rows and --repeat must be positive")
    os.makedirs(args.out, exist_ok=True)
    rows = make_rows(args.rows)
    results = run_case(rows, args.repeat)
    csv_path = os.path.join(args.out, "results.csv")
    json_path = os.path.join(args.out, "results.json")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump({"benchmark": "kira-data-formats-v1", "python": __import__("sys").version,
                   "rows": args.rows, "repeat": args.repeat, "results": results},
                  f, ensure_ascii=False, indent=2)
    print("KIRA data format benchmark (host-side; round-trip checked)")
    print(f"rows={args.rows} repeat={args.repeat}")
    print(f"{'format':20} {'bytes':>12} {'enc ms':>10} {'dec ms':>10} {'roundtrip':>10}")
    for r in results:
        print(f"{r['format']:20} {r['bytes']:12d} {r['encode_ms']:10.3f} "
              f"{r['decode_ms']:10.3f} {r['roundtrip']:>10}")
    print(f"CSV: {csv_path}")
    print(f"JSON: {json_path}")


if __name__ == "__main__":
    main()
