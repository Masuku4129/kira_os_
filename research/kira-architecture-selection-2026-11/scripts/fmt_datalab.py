#!/usr/bin/env python3 -I
"""[E] Форматы данных и сжатие на данных KIRA (повтор Data Format Lab на своих наборах). Запуск: taskset -c 0 python3 -I scripts/fmt_datalab.py
Наборы: K1 образ kfs (kira.disk целиком, байты); K2 список файлов kfs (kfstool ls -> записи path,size,type); K3 текст журнала (serial загрузки KIRA = то, что кладёт klog);
K4 манифест (PROG_MANIFEST: имя + SHA-256, 26 шт.) + индекс драйверов; S1 синтетика: 100000 строк телеметрии (ts, sensor, value, flags) - явно синтетика.
Форматы: JSON, CBOR (cbor2), MessagePack, Fixed Binary (struct), Varint (LEB128 + zigzag-delta по строкам), Columnar (колонки подряд), Columnar+Delta+Varint; сжатие поверх: zlib-6, zstd-3, zstd-19, lz4, xz-6.
Размеры - детерминированы (rep=1); время кодирования/декодирования - медиана >=9 повторов (CPython: сравнивать только внутри таблицы)."""
import os, sys, re, json, struct, time, zlib, lzma, statistics, subprocess, random, glob
import cbor2, msgpack, zstandard, lz4.frame
import numpy as np
ART = os.environ.get('FMT_ART', '/home/claude/kira_os_')
HERE = os.path.dirname(os.path.abspath(__file__)); DATA = os.path.join(HERE, '..', 'data', 'results_fmt.csv'); LOG = open(os.path.join(HERE, '..', 'logs', 'fmt_datalab.log'), 'w')
def out(s=''): print(s); LOG.write(s + '\n'); LOG.flush()
def rec(cand, wl, metric, rep, val, unit, note=''): open(DATA, 'a').write('fmt_data,x86_64,vm-core0,%s,%s,%s,%d,%s,%s,0,"%s"\n' % (cand, wl, metric, rep, val, unit, note))
# ---------- кодеки ----------
def zz(v): return (v << 1) ^ (v >> 63)
def uz(u): return (u >> 1) ^ -(u & 1)
def lebs(vals):
    o = bytearray()
    for v in vals:
        while v >= 0x80: o.append((v & 0x7f) | 0x80); v >>= 7
        o.append(v)
    return bytes(o)
def unleb(b, n):
    r = []; i = 0
    for _ in range(n):
        s = 0; v = 0
        while True:
            c = b[i]; i += 1; v |= (c & 0x7f) << s; s += 7
            if c < 0x80: break
        r.append(v)
    return r, i
# таблица = список колонок: ('i', ширина_байт, значения) | ('s', строки) | ('b', байты32)
def enc_fixed(cols):
    n = len(cols[0][-1]); o = bytearray(struct.pack('<I', n))
    arrs = []
    for c in cols:
        if c[0] == 'i': arrs.append(np.asarray(c[2], dtype={1: '<u1', 2: '<u2', 4: '<u4', 8: '<u8'}[c[1]]))
        elif c[0] == 'b': arrs.append(np.frombuffer(b''.join(c[1]), dtype='V32'))
        else: w = max(len(s.encode()) for s in c[1]) or 1; arrs.append(np.array([s.encode().ljust(w, b'\0') for s in c[1]], dtype='S%d' % w))
    dt = np.dtype([('f%d' % i, a.dtype) for i, a in enumerate(arrs)]); t = np.zeros(n, dtype=dt)
    for i, a in enumerate(arrs): t['f%d' % i] = a
    return bytes(o) + t.tobytes()   # строки: поле фиксированной ширины (макс. длина) - так делает Fixed Binary
def enc_columnar(cols):
    n = len(cols[0][-1]); o = bytearray(struct.pack('<I', n))
    for c in cols:
        if c[0] == 'i': o += np.asarray(c[2], dtype={1: '<u1', 2: '<u2', 4: '<u4', 8: '<u8'}[c[1]]).tobytes()
        elif c[0] == 'b': o += b''.join(c[1])
        else: bs = [s.encode() for s in c[1]]; o += lebs([len(x) for x in bs]) + b''.join(bs)
    return bytes(o)
def enc_columnar_dv(cols):
    n = len(cols[0][-1]); o = bytearray(struct.pack('<I', n))
    for c in cols:
        if c[0] == 'i':
            a = np.asarray(c[2], dtype=np.int64); d = np.diff(a, prepend=0); o += lebs([int(zz(int(x))) for x in d])      # дельта к предыдущему + zigzag + LEB128
        elif c[0] == 'b': o += b''.join(c[1])
        else: bs = [s.encode() for s in c[1]]; o += lebs([len(x) for x in bs]) + b''.join(bs)
    return bytes(o)
def enc_varint_rows(cols):
    n = len(cols[0][-1]); o = bytearray(struct.pack('<I', n)); prev = [0] * len(cols)
    for r in range(n):
        for j, c in enumerate(cols):
            if c[0] == 'i': v = int(c[2][r]); o += lebs([zz(v - prev[j])]); prev[j] = v
            elif c[0] == 'b': o += c[1][r]
            else: b = c[1][r].encode(); o += lebs([len(b)]) + b
    return bytes(o)
def dec_columnar_dv(b, spec):
    n = struct.unpack_from('<I', b)[0]; p = 4; res = []
    for c in spec:
        if c == 'i': v, k = unleb(b[p:], n); p += k; a = np.cumsum([uz(x) for x in v]); res.append(a)
        elif c == 'b': res.append([b[p + 32 * i:p + 32 * i + 32] for i in range(n)]); p += 32 * n
        else: ln, k = unleb(b[p:], n); p += k; ss = []; 
        if c == 's':
            for L in ln: ss.append(b[p:p + L]); p += L
            res.append(ss)
    return res
def dec_columnar_fixed(b, cols):
    n = struct.unpack_from('<I', b)[0]; p = 4; res = []
    for c in cols:
        if c[0] == 'i': w = c[1]; res.append(np.frombuffer(b, dtype={1: '<u1', 2: '<u2', 4: '<u4', 8: '<u8'}[w], count=n, offset=p)); p += w * n
        elif c[0] == 'b': res.append([b[p + 32 * i:p + 32 * i + 32] for i in range(n)]); p += 32 * n
        else: ln, k = unleb(b[p:], n); p += k; ss = []; 
        if c[0] == 's':
            for L in ln: ss.append(b[p:p + L]); p += L
            res.append(ss)
    return res
def rows_of(cols): n = len(cols[0][-1]); return [[(c[-1][r] if c[0] != 'i' else int(c[-1][r])) for c in cols] for r in range(n)]
def names(cols): return ['c%d' % i for i in range(len(cols))]
def enc_json(cols): return json.dumps([dict(zip(names(cols), [x.hex() if isinstance(x, bytes) else x for x in r])) for r in rows_of(cols)], separators=(',', ':')).encode()
def enc_json_arr(cols): return json.dumps([[x.hex() if isinstance(x, bytes) else x for x in r] for r in rows_of(cols)], separators=(',', ':')).encode()
def enc_cbor(cols): return cbor2.dumps([dict(zip(names(cols), r)) for r in rows_of(cols)])
def enc_cbor_arr(cols): return cbor2.dumps(rows_of(cols))
def enc_msgpack(cols): return msgpack.packb([dict(zip(names(cols), r)) for r in rows_of(cols)])
def enc_msgpack_arr(cols): return msgpack.packb(rows_of(cols))
FORMATS = [('json-map', enc_json), ('json-array', enc_json_arr), ('cbor-map', enc_cbor), ('cbor-array', enc_cbor_arr), ('msgpack-map', enc_msgpack), ('msgpack-array', enc_msgpack_arr),
           ('fixed-binary', enc_fixed), ('varint-rows', enc_varint_rows), ('columnar', enc_columnar), ('columnar-delta-varint', enc_columnar_dv)]
COMPS = [('zlib-6', lambda d: zlib.compress(d, 6), zlib.decompress), ('zstd-3', lambda d: zstandard.ZstdCompressor(level=3).compress(d), lambda c: zstandard.ZstdDecompressor().decompress(c)),
         ('zstd-19', lambda d: zstandard.ZstdCompressor(level=19).compress(d), lambda c: zstandard.ZstdDecompressor().decompress(c)), ('lz4', lambda d: lz4.frame.compress(d), lz4.frame.decompress),
         ('xz-6', lambda d: lzma.compress(d, preset=6), lzma.decompress)]
def med_time(fn, reps=9):
    ts = []
    for _ in range(reps + 1):
        t = time.perf_counter(); fn(); ts.append((time.perf_counter() - t) * 1e3)
    return statistics.median(ts[1:]), min(ts[1:]), max(ts[1:])
def lab(tag, cols, desc):
    n = len(cols[0][-1]); out('\n== %s: %s; строк %d' % (tag, desc, n)); rec('dataset', tag, 'rows', 1, n, 'count', desc)
    # проверка корректности: кодирование -> декодирование columnar-dv и fixed совпадает с исходными колонками
    dv = enc_columnar_dv(cols); spec = [c[0] for c in cols]; back = dec_columnar_dv(dv, spec)
    for c, bk in zip(cols, back):
        if c[0] == 'i': assert [int(x) for x in bk] == [int(x) for x in c[2]], 'columnar-dv roundtrip ' + tag
        elif c[0] == 's': assert [x.decode() for x in bk] == c[1], 'columnar-dv str ' + tag
        else: assert list(bk) == list(c[1]), 'columnar-dv bytes ' + tag
    cl = enc_columnar(cols); back = dec_columnar_fixed(cl, cols)
    for c, bk in zip(cols, back):
        if c[0] == 'i': assert [int(x) for x in bk] == [int(x) for x in c[2]], 'columnar roundtrip ' + tag
    rawsz = None; rows_out = []
    for fn, f in FORMATS:
        d = f(cols); rec(fn, tag, 'size', 1, len(d), 'B')
        line = '  %-22s raw %9d' % (fn, len(d)); cs = {}
        for cn, c, dc in COMPS:
            z = c(d); assert dc(z) == d; cs[cn] = len(z); rec(fn + '+' + cn, tag, 'size', 1, len(z), 'B'); line += '  %s %d' % (cn, len(z))
        out(line)
    # время: кодирование и декодирование ключевых вариантов (CPython; относительные величины)
    for fn, f in FORMATS:
        if n > 20000 and fn in ('json-map', 'cbor-map', 'msgpack-map', 'json-array'): continue
        m, lo, hi = med_time(lambda: f(cols), 9); rec(fn, tag, 'encode_time', 1, '%.3f' % m, 'ms', 'медиана 9, CPython+numpy; min %.3f max %.3f' % (lo, hi)); out('  encode %-22s med %9.3f ms (min %.3f max %.3f)' % (fn, m, lo, hi))
    for cn, c, dc in COMPS:
        d = enc_fixed(cols); z = c(d); m, lo, hi = med_time(lambda: dc(z), 9); rec('fixed-binary+' + cn, tag, 'decompress_time', 1, '%.3f' % m, 'ms', 'медиана 9'); out('  decompress fixed+%-8s med %9.3f ms' % (cn, m))
    for nm, enc, dec in (('cbor-array', enc_cbor_arr, cbor2.loads), ('msgpack-array', enc_msgpack_arr, lambda b: msgpack.unpackb(b)), ('json-array', enc_json_arr, json.loads)):
        if n > 200000: continue
        d = enc(cols); m, lo, hi = med_time(lambda: dec(d), 9); rec(nm, tag, 'decode_time', 1, '%.3f' % m, 'ms', 'медиана 9'); out('  decode %-22s med %9.3f ms' % (nm, m))
    d = enc_columnar_dv(cols); m, lo, hi = med_time(lambda: dec_columnar_dv(d, spec), 9); rec('columnar-delta-varint', tag, 'decode_time', 1, '%.3f' % m, 'ms', 'медиана 9, чистый Python'); out('  decode columnar-delta-varint med %9.3f ms (чистый Python)' % m)
    d = enc_columnar(cols); m, lo, hi = med_time(lambda: dec_columnar_fixed(d, cols), 9); rec('columnar', tag, 'decode_time', 1, '%.3f' % m, 'ms', 'медиана 9, numpy frombuffer'); out('  decode columnar(fixed ints) med %9.3f ms (numpy)' % m)
# ---------- наборы ----------
def kfs_listing():
    kt = os.path.join(HERE, '..', '..', '..', 'tools', 'kfstool')
    if not os.path.exists(kt): kt = ART + '/tools/kfstool'
    res = []
    def walk(p):
        o = subprocess.run([kt, ART + '/kira.disk', 'ls', p], capture_output=True, text=True).stdout.splitlines()
        for l in o:
            m = re.match(r'([d-])\s+(\d+)\s+(\S+)', l)
            if not m: continue
            res.append((p.rstrip('/') + '/' + m.group(3), int(m.group(2)), 2 if m.group(1) == 'd' else 1))
            if res[-1][2] == 2 and len(res) < 400: walk(res[-1][0])
    walk('/'); return res
def main():
    open(DATA, 'a').close()
    # K1: образ kfs целиком
    img = open(ART + '/kira.disk', 'rb').read(); out('== K1: kira.disk целиком, %d Б (KiraFS v2, блок 4 КБ, большая часть нулей)' % len(img)); rec('raw', 'K1_kfs_image', 'size', 1, len(img), 'B')
    nz = sum(1 for i in range(0, len(img), 4096) if any(img[i:i + 4096])); out('  ненулевых блоков 4 КБ: %d из %d' % (nz, len(img) // 4096)); rec('raw', 'K1_kfs_image', 'nonzero_blocks', 1, nz, 'count')
    for cn, c, dc in COMPS + [('gzip-9', lambda d: zlib.compress(d, 9), zlib.decompress)]:
        z = c(img); assert dc(z) == img; m, lo, hi = med_time(lambda: dc(z), 9); out('  %-8s %9d Б (%.2f%%), распаковка med %.2f ms' % (cn, len(z), 100 * len(z) / len(img), m)); rec(cn, 'K1_kfs_image', 'size', 1, len(z), 'B'); rec(cn, 'K1_kfs_image', 'decompress_time', 1, '%.3f' % m, 'ms')
    # K2: листинг kfs
    ls = kfs_listing(); out('\n(листинг kfs: %d записей)' % len(ls))
    if len(ls) >= 5: lab('K2_kfs_listing', [('s', [r[0] for r in ls]), ('i', 8, [r[1] for r in ls]), ('i', 1, [r[2] for r in ls])], 'путь, размер, тип (kfstool ls kira.disk)')
    else: out('K2: NOT RUN (kfstool ls вернул %d записей)' % len(ls)); rec('dataset', 'K2_kfs_listing', 'status', 1, 'NOT RUN', 'flag', 'kfstool ls не дал записей')
    # K3: журнал загрузки (serial)
    logs = sorted(glob.glob(os.path.join(HERE, '..', 'logs', 'fmt_klog_full.log'))) or sorted(glob.glob(os.path.join(HERE, '..', 'logs', 'fmt_boot_bios_*.log')))
    if logs:
        lines = open(logs[0], 'rb').read().decode('utf-8', 'replace').replace('\r', '').split('\n'); lines = [l for l in lines if l]
        tags = [(re.match(r'\[(KIRA [A-Z0-9 ]+)\]', l).group(1) if re.match(r'\[(KIRA [A-Z0-9 ]+)\]', l) else '') for l in lines]
        msgs = [re.sub(r'^\[[^\]]*\]\s*', '', l) for l in lines]
        lab('K3_boot_log', [('i', 4, list(range(len(lines)))), ('s', tags), ('s', msgs)], 'serial загрузки (то, что хранит klog): номер, тег [KIRA ...], сообщение (%s)' % os.path.basename(logs[0]))
        txt = '\n'.join(lines).encode(); out('  журнал как текст: %d Б' % len(txt)); rec('raw-text', 'K3_boot_log', 'size', 1, len(txt), 'B')
        for cn, c, dc in COMPS: z = c(txt); out('  текст+%-8s %d' % (cn, len(z))); rec('text+' + cn, 'K3_boot_log', 'size', 1, len(z), 'B')
    else: out('K3: NOT RUN (нет logs/fmt_boot_bios_*.log: сначала scripts/fmt_boot.py bios)'); rec('dataset', 'K3_boot_log', 'status', 1, 'NOT RUN', 'flag', 'нет лога загрузки')
    # K4: манифест доверенных программ (x86-64 manifest.h сборка не сохраняет; берётся ARM64-манифест того же формата - хеши ARM64-ELF)
    mf = open(ART + '/build-arm64/manifest.h').read(); ent = re.findall(r'\{ "([^"]+)", \{([^}]*)\} \}', mf)
    hs = [bytes(int(x, 16) for x in e[1].replace(' ', '').split(',') if x) for e in ent]
    lab('K4_manifest', [('s', [e[0] for e in ent]), ('b', hs)], 'PROG_MANIFEST: имя + SHA-256 (%d программ; из build-arm64/manifest.h)' % len(ent))
    # S1: синтетика
    random.seed(7); n = 100000; ts = np.cumsum(np.random.default_rng(7).integers(1, 20, n)) + 1_700_000_000_000; sid = np.random.default_rng(8).integers(0, 12, n); val = np.cumsum(np.random.default_rng(9).integers(-3, 4, n)) + 2000; val = np.clip(val, 0, 65535); fl = np.random.default_rng(10).choice([0, 0, 0, 0, 1, 2, 4], n)
    lab('S1_synthetic_telemetry', [('i', 8, ts.tolist()), ('i', 1, sid.tolist()), ('i', 2, val.tolist()), ('i', 1, fl.tolist())], 'СИНТЕТИКА: ts мс (дельта 1..19), sensor 0..11, value random walk u16, flags')
main()
