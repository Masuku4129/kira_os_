#!/usr/bin/env python3 -I
"""[E] Размеры артефактов KIRA в разных представлениях: ELF / strip / плоский образ / сжатие; секции; число перемещений;
время распаковки (>=9 повторов). Запуск: taskset -c 0 python3 -I scripts/fmt_sizes.py. Пишет data/results_fmt.csv, logs/fmt_sizes.log."""
import os, subprocess, sys, time, gzip, bz2, lzma, glob, statistics, tempfile, zlib
import zstandard, lz4.frame
ART = os.environ.get('FMT_ART', '/home/claude/kira_os_')
HERE = os.path.dirname(os.path.abspath(__file__)); DATA = os.path.join(HERE, '..', 'data', 'results_fmt.csv')
LOG = open(os.path.join(HERE, '..', 'logs', 'fmt_sizes.log'), 'w')
if not os.path.exists(DATA): open(DATA, 'w').write('exp,arch,env,candidate,workload,metric,rep,value,unit,exit_code,note\n')
def out(s): print(s); LOG.write(s + '\n'); LOG.flush()
def rec(arch, cand, wl, metric, rep, val, unit, note=''):
    open(DATA, 'a').write('fmt_sizes,%s,vm-core0,%s,%s,%s,%s,%s,%s,0,"%s"\n' % (arch, cand, wl, metric, rep, val, unit, note))
def sh(*a): return subprocess.check_output(a, text=True)
def rd(p): return open(p, 'rb').read()
T = tempfile.mkdtemp(prefix='fmtsz', dir=os.environ.get('TMPDIR'))
COMP = {
 'gzip-9':  (lambda d: gzip.compress(d, 9, mtime=0), gzip.decompress),
 'bzip2-9': (lambda d: bz2.compress(d, 9), bz2.decompress),
 'xz-9e':   (lambda d: lzma.compress(d, preset=9 | lzma.PRESET_EXTREME), lzma.decompress),
 'zstd-19': (lambda d: zstandard.ZstdCompressor(level=19).compress(d), lambda c: zstandard.ZstdDecompressor().decompress(c)),
 'zstd-3':  (lambda d: zstandard.ZstdCompressor(level=3).compress(d), lambda c: zstandard.ZstdDecompressor().decompress(c)),
 'lz4-hc':  (lambda d: lz4.frame.compress(d, compression_level=12), lz4.frame.decompress),
}
def nrel(path):  # число записей перемещений во всех секциях .rela*
    t = sh('readelf', '-rW', path)
    return sum(1 for l in t.splitlines() if l[:1] in '0123456789abcdef' and len(l.split()) >= 4 and l.split()[0].isalnum() and l.split()[2].startswith('R_'))
def compress_table(arch, name, data):
    out('  %-28s raw=%8d' % (name, len(data)))
    rec(arch, 'raw', name, 'size', 1, len(data), 'B')
    for cn, (c, d) in COMP.items():
        z = c(data); assert d(z) == data, cn + ' roundtrip'
        ts = []
        for _ in range(11):
            t0 = time.perf_counter(); d(z); ts.append((time.perf_counter() - t0) * 1e3)
        ts = ts[2:]  # прогрев 2
        out('    %-8s %8d (%5.1f%%)  распаковка med %.3f ms (n=9, min %.3f max %.3f)' % (cn, len(z), 100 * len(z) / len(data), statistics.median(ts), min(ts), max(ts)))
        rec(arch, cn, name, 'size', 1, len(z), 'B', 'сжатое; roundtrip проверен')
        for i, v in enumerate(ts): rec(arch, cn, name, 'decompress_time', i + 1, '%.4f' % v, 'ms', 'python-биндинг в одном процессе')
# --- ядро x86-64
out('== ядро x86-64: %s/kira.bin' % ART)
k = os.path.join(ART, 'kira.bin'); ks = os.path.join(T, 'kira.stripped'); kf = os.path.join(T, 'kira.flat'); kd = os.path.join(T, 'kira.sdbg')
subprocess.check_call(['strip', '-s', '-o', ks, k]); subprocess.check_call(['objcopy', '-O', 'binary', k, kf])
subprocess.check_call(['llvm-objcopy', '--strip-all', '-o', kd, k]) if False else None
out('  ELF (с .symtab/.strtab) %d; strip -s %d; плоский (objcopy -O binary) %d' % (os.path.getsize(k), os.path.getsize(ks), os.path.getsize(kf)))
rec('x86_64', 'elf', 'kernel', 'size', 1, os.path.getsize(k), 'B'); rec('x86_64', 'elf-strip', 'kernel', 'size', 1, os.path.getsize(ks), 'B'); rec('x86_64', 'flat-binary', 'kernel', 'size', 1, os.path.getsize(kf), 'B', 'objcopy -O binary: .rodata+.text+.data, без .bss')
rec('x86_64', 'elf', 'kernel', 'relocations', 1, nrel(k), 'count', 'ET_EXEC без -pie: перемещений нет')
for nm, p in (('kernel.elf', k), ('kernel.elf-stripped', ks), ('kernel.flat', kf)): compress_table('x86_64', nm, rd(p))
# сколько весит таблица символов и сколько заголовки
sym = os.path.getsize(k) - os.path.getsize(ks)
out('  разница ELF - strip (=.symtab+.strtab+.comment+заголовки секций) = %d Б (%.1f%% файла)' % (sym, 100 * sym / os.path.getsize(k)))
rec('x86_64', 'elf', 'kernel', 'symtab_overhead', 1, sym, 'B', 'ELF минус strip -s')
# --- ISO/BIOS/UEFI образы (контейнеры вокруг того же ядра)
for f, arch in (('kira.iso', 'x86_64'), ('kira-bios.img', 'x86_64'), ('kira-uefi.img', 'x86_64'), ('kira-arm64.elf', 'aarch64'), ('kira-arm64.img', 'aarch64'), ('bios-stage1.bin', 'x86_64'), ('bios-stage2.bin', 'x86_64'), ('kira.disk', 'x86_64')):
    s = os.path.getsize(os.path.join(ART, f)); out('  образ %-16s %d Б' % (f, s)); rec(arch, 'image', f, 'size', 1, s, 'B')
# --- ARM64
out('== ARM64: kira-arm64.elf (static-pie) vs kira-arm64.img (заголовок Image + плоский)')
ae = os.path.join(ART, 'kira-arm64.elf'); ai = os.path.join(ART, 'kira-arm64.img')
rela = sh('readelf', '-rW', ae); nrelative = rela.count('R_AARCH64_RELATIVE')
out('  R_AARCH64_RELATIVE записей: %d (применяются ядром в boot.S)' % nrelative); rec('aarch64', 'static-pie', 'kernel', 'relocations', 1, nrelative, 'count', 'R_AARCH64_RELATIVE')
aes = os.path.join(T, 'a64.stripped')
if subprocess.call(['llvm-objcopy', '--strip-all', ae, aes], stderr=subprocess.DEVNULL) == 0 and os.path.exists(aes):
    out('  arm64.elf strip (llvm-objcopy --strip-all): %d' % os.path.getsize(aes)); rec('aarch64', 'elf-strip', 'kernel', 'size', 1, os.path.getsize(aes), 'B')
for nm, p in (('arm64.elf', ae), ('arm64.Image', ai)): compress_table('aarch64', nm, rd(p))
# --- userland
out('== userland x86-64: ET_EXEC, 3 PT_LOAD (R-X, R--, RW-), база 0x8000000000')
els = sorted(glob.glob(os.path.join(ART, 'userland', '*.elf'))); tot = tots = 0; allbytes = b''
for e in els:
    st = os.path.join(T, os.path.basename(e) + '.s'); subprocess.check_call(['strip', '-s', '-o', st, e])
    s, ss = os.path.getsize(e), os.path.getsize(st); tot += s; tots += ss; allbytes += rd(st)
    nm = os.path.basename(e)[:-4]; rec('x86_64', 'elf', 'user/' + nm, 'size', 1, s, 'B'); rec('x86_64', 'elf-strip', 'user/' + nm, 'size', 1, ss, 'B')
    out('  %-10s ELF %7d  strip %7d  (-%.0f%%)' % (nm, s, ss, 100 * (s - ss) / s))
out('  ИТОГО %d программ: ELF %d, strip %d' % (len(els), tot, tots)); rec('x86_64', 'elf', 'user/ALL', 'size', 1, tot, 'B', '%d файлов' % len(els)); rec('x86_64', 'elf-strip', 'user/ALL', 'size', 1, tots, 'B')
for nm in ('edit', 'kcc', 'fetch'):
    p = os.path.join(ART, 'userland', nm + '.elf'); st = os.path.join(T, nm + '.elf.s'); out(' -- %s' % nm); compress_table('x86_64', 'user/%s.elf-strip' % nm, rd(st))
# распределение секций userland/kcc.elf
out(sh('readelf', '-SW', os.path.join(ART, 'userland', 'kcc.elf')))
# --- объектные файлы (ET_REL): ядро
objs = sorted(f for f in glob.glob(os.path.join(ART, '*.o')) + glob.glob(os.path.join(ART, 'arch/x86_64/*.o')) if os.path.isfile(f))
tot_o = sum(os.path.getsize(o) for o in objs); tot_r = 0
for o in objs:
    try: tot_r += nrel(o)
    except Exception: pass
out('== ET_REL ядра x86-64: %d объектов, %d Б, %d записей перемещений (readelf -r)' % (len(objs), tot_o, tot_r))
rec('x86_64', 'et_rel', 'kernel-objects', 'size', 1, tot_o, 'B', '%d .o' % len(objs)); rec('x86_64', 'et_rel', 'kernel-objects', 'relocations', 1, tot_r, 'count')
# --- WASM-модули
ws = sorted(glob.glob(os.path.join(ART, 'build-wasm', '*.wasm')))
for w in ws: out('  wasm %-12s %d Б' % (os.path.basename(w), os.path.getsize(w))); rec('wasm32', 'wasm', os.path.basename(w), 'size', 1, os.path.getsize(w), 'B')
# --- kdrv-пакет: накладные формата
out('== .kdrv: заголовок 136 + ELF; .rec 216; .sig 144 => накладные поверх ELF = %d Б' % (136 + 216 + 144)); rec('x86_64', 'kdrv+rec+sig', 'overhead', 'size', 1, 136 + 216 + 144, 'B', 'по static_assert')
# kdrv пример
drv = [f for f in glob.glob(os.path.join(ART, 'userland', 'drvedu.elf'))]
if drv: out('  drvedu.elf: %d Б; пакет = %d Б' % (os.path.getsize(drv[0]), os.path.getsize(drv[0]) + 136))
