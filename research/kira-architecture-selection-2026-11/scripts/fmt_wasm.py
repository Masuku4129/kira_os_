#!/usr/bin/env python3 -I
"""[E] WASM как переносимая песочница: 3 нагрузки (sha256, sort, matmul) - native clang -O2 vs wasmtime (Cranelift) vs интерпретатор KIRA (userland/wasm.c, хост-обвязка tests/wasm_host.c; с JIT x86-64 и без).
Запуск: taskset -c 0 python3 -I scripts/fmt_wasm.py [REPS=9]. Корректность до скорости: контрольные суммы всех четырёх исполнителей должны совпасть.
Пишет results_fmt.csv (exp=fmt_wasm), logs/fmt_wasm.log. Сборка модулей - тем же способом, что userland/wasm/build.sh (формат kw.h: импорт "kira", экспорт kmain)."""
import os, sys, subprocess, time, statistics, tempfile, shutil, resource
import wasmtime
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..')); DATA = os.path.join(HERE, '..', 'data', 'results_fmt.csv')
LOG = open(os.path.join(HERE, '..', 'logs', 'fmt_wasm.log'), 'w'); REPS = int(sys.argv[1]) if len(sys.argv) > 1 else 9
T = tempfile.mkdtemp(prefix='fmtwasm', dir=os.environ.get('TMPDIR'))
def out(s=''): print(s); LOG.write(s + '\n'); LOG.flush()
def rec(cand, wl, metric, rep, val, unit, note='', rc=0): open(DATA, 'a').write('fmt_wasm,x86_64,vm-core0,%s,%s,%s,%d,%s,%s,%d,"%s"\n' % (cand, wl, metric, rep, val, unit, rc, note))
def sh(*a, **k): return subprocess.run(a, check=True, capture_output=True, text=True, **k)
RS = {1: 400, 2: 3, 3: 100}; NAMES = {1: 'sha256', 2: 'sort', 3: 'matmul'}    # R подобраны так, чтобы native ~десятки мс
if os.environ.get('FMT_WASM_R'): RS = {i + 1: int(x) for i, x in enumerate(os.environ['FMT_WASM_R'].split(','))}
WF = ['--target=wasm32', '-O2', '-nostdlib', '-ffreestanding', '-fno-builtin', '-I' + REPO + '/userland/wasm']
sh('clang', *WF, '-c', REPO + '/userland/wasm/kwlib.c', '-o', T + '/kwlib.o')
sh('gcc', '-O2', '-o', T + '/fmt_rss', REPO + '/research/kira-architecture-selection-2026-11/scripts/fmt_rss.c')
sh('gcc', '-O2', '-DKIRA_HOST_TEST', '-w', '-I' + REPO, '-o', T + '/wasm_host', REPO + '/tests/wasm_host.c', '-lm')
def wait4(cmd, env=None):
    t0 = time.perf_counter(); r = subprocess.run([T + '/fmt_rss'] + cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env); dt = (time.perf_counter() - t0) * 1e3
    o = r.stdout.decode(errors='replace'); rss = 0
    for l in o.splitlines():
        if l.startswith('MAXRSS_KB'): rss = int(l.split()[1])
    return o, dt, rss, r.returncode
def checksum(s):
    for l in s.splitlines():
        if l.startswith('checksum '): return l.split()[1]
    return None
class Run:  # wasmtime
    def __init__(self, path): self.path = path
    def go(self):
        t0 = time.perf_counter(); eng = wasmtime.Engine(); mod = wasmtime.Module.from_file(eng, self.path); t1 = time.perf_counter()
        store = wasmtime.Store(eng); buf = []
        linker = wasmtime.Linker(eng)
        def write(caller, p, n):
            mem = caller.get('memory'); buf.append(bytes(mem.read(caller, p, p + n))); return n
        for imp in mod.imports:
            if imp.name == 'write': linker.define_func('kira', 'write', wasmtime.FuncType([wasmtime.ValType.i32(), wasmtime.ValType.i32()], [wasmtime.ValType.i32()]), write, access_caller=True)
            else: raise RuntimeError('нереализованный импорт ' + imp.name)
        inst = linker.instantiate(store, mod); t2 = time.perf_counter()
        inst.exports(store)['kmain'](store); t3 = time.perf_counter()
        return b''.join(buf).decode(), (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3
def med(v): return statistics.median(v)
out('wasmtime (python) %s; host_clang: %s' % (getattr(wasmtime, '__version__', 'n/a') if hasattr(wasmtime, '__version__') else 'пакет wasmtime', sh('clang', '--version').stdout.splitlines()[0]))
for wl in (1, 2, 3):
    nm = NAMES[wl]; r = RS[wl]; out('\n== %s (R=%d)' % (nm, r)); d = ['-DWL=%d' % wl, '-DR=%d' % r]
    sh('clang', '-O2', '-DKW_NATIVE', *d, '-w', '-I' + REPO + '/userland/wasm', '-o', T + '/n_' + nm, REPO + '/research/kira-architecture-selection-2026-11/scripts/fmt_wasm_wl.c')
    sh('clang', *WF, *d, '-c', REPO + '/research/kira-architecture-selection-2026-11/scripts/fmt_wasm_wl.c', '-o', T + '/w_' + nm + '.o')
    sh('wasm-ld', '--no-entry', '--export=kmain', '-z', 'stack-size=65536', '--initial-memory=2097152', T + '/w_' + nm + '.o', T + '/kwlib.o', '-o', T + '/' + nm + '.wasm')
    sh('strip', '-s', '-o', T + '/n_' + nm + '.s', T + '/n_' + nm)
    sw, sn, sns = os.path.getsize(T + '/' + nm + '.wasm'), os.path.getsize(T + '/n_' + nm), os.path.getsize(T + '/n_' + nm + '.s')
    out('  размер: wasm %d Б; native ELF (динамический, glibc) %d, strip %d' % (sw, sn, sns)); rec('wasm', nm, 'module_size', 1, sw, 'B'); rec('native-clang-O2', nm, 'binary_size', 1, sns, 'B', 'ELF strip, динамическая линковка с glibc - справка')
    # корректность
    ref = checksum(wait4([T + '/n_' + nm])[0]); k_jit = checksum(wait4([T + '/wasm_host', T + '/' + nm + '.wasm'])[0])
    k_int = checksum(wait4([T + '/wasm_host', T + '/' + nm + '.wasm'], env=dict(os.environ, WASM_NOJIT='1'))[0]); wt = checksum(Run(T + '/' + nm + '.wasm').go()[0])
    out('  контрольные суммы: native %s | KIRA-jit %s | KIRA-interp %s | wasmtime %s' % (ref, k_jit, k_int, wt))
    ok = ref is not None and ref == k_jit == k_int == wt
    rec('all', nm, 'checksum_equal', 1, 'PASS' if ok else 'FAIL', 'flag', 'native=%s jit=%s interp=%s wasmtime=%s' % (ref, k_jit, k_int, wt), 0 if ok else 1)
    if not ok: out('  FAIL: контрольные суммы разошлись - скорость не измеряется'); continue
    res = {}
    for cand, cmd, env in (('native-clang-O2', [T + '/n_' + nm], None), ('kira-wasm-jit', [T + '/wasm_host', T + '/' + nm + '.wasm'], None), ('kira-wasm-interp', [T + '/wasm_host', T + '/' + nm + '.wasm'], dict(os.environ, WASM_NOJIT='1'))):
        ts = []; rss = []
        for i in range(REPS + 2):
            o, dt, rs, st = wait4(cmd, env)
            if i >= 2: ts.append(dt); rss.append(rs)      # прогрев 2
        res[cand] = ts
        for i, v in enumerate(ts): rec(cand, nm, 'wall_process', i + 1, '%.2f' % v, 'ms', 'wall процесса целиком, ядро 0')
        rec(cand, nm, 'peak_rss', 1, max(rss), 'KiB', 'ru_maxrss потомка через fmt_rss (wait4), максимум из повторов')
        out('  %-18s wall med %9.2f ms (min %.2f max %.2f, n=%d); peak RSS %d KiB' % (cand, med(ts), min(ts), max(ts), len(ts), max(rss)))
    # wasmtime: фазы
    ph = {'compile': [], 'instantiate': [], 'run': []}
    for i in range(REPS + 2):
        o, c, ins, ru = Run(T + '/' + nm + '.wasm').go()
        if i >= 2: ph['compile'].append(c); ph['instantiate'].append(ins); ph['run'].append(ru)
    for k, v in ph.items():
        for i, x in enumerate(v): rec('wasmtime-cranelift', nm, k + '_time', i + 1, '%.3f' % x, 'ms', 'в процессе Python; новый Engine+Module на каждый повтор' if k == 'compile' else 'в процессе Python')
        out('  wasmtime %-12s med %9.3f ms (min %.3f max %.3f)' % (k, med(v), min(v), max(v)))
    nat = med(res['native-clang-O2'])
    out('  ОТНОШЕНИЯ к native (wall процесса; native включает старт процесса ~ms): KIRA-jit %.1fx, KIRA-interp %.1fx, wasmtime run-only %.2fx' % (med(res['kira-wasm-jit']) / nat, med(res['kira-wasm-interp']) / nat, med(ph['run']) / nat))
    for cand, v in (('kira-wasm-jit', med(res['kira-wasm-jit']) / nat), ('kira-wasm-interp', med(res['kira-wasm-interp']) / nat), ('wasmtime-cranelift', med(ph['run']) / nat)): rec(cand, nm, 'slowdown_vs_native', 1, '%.2f' % v, 'x', 'медиана/медиана; native wall включает старт процесса')
# старт: R=0 как «пустая» нагрузка
out('\n== старт (sha256, R=0: заполнение буфера + один вызов kmain)')
d = ['-DWL=1', '-DR=0']
sh('clang', '-O2', '-DKW_NATIVE', *d, '-w', '-I' + REPO + '/userland/wasm', '-o', T + '/n_hello', REPO + '/research/kira-architecture-selection-2026-11/scripts/fmt_wasm_wl.c')
sh('clang', *WF, *d, '-c', REPO + '/research/kira-architecture-selection-2026-11/scripts/fmt_wasm_wl.c', '-o', T + '/w_hello.o'); sh('wasm-ld', '--no-entry', '--export=kmain', '-z', 'stack-size=65536', '--initial-memory=2097152', T + '/w_hello.o', T + '/kwlib.o', '-o', T + '/hello.wasm')
for cand, cmd, env in (('native-clang-O2', [T + '/n_hello'], None), ('kira-wasm-jit', [T + '/wasm_host', T + '/hello.wasm'], None), ('kira-wasm-interp', [T + '/wasm_host', T + '/hello.wasm'], dict(os.environ, WASM_NOJIT='1'))):
    ts = [wait4(cmd, env)[1] for _ in range(REPS + 2)][2:]
    for i, v in enumerate(ts): rec(cand, 'startup', 'wall_process', i + 1, '%.2f' % v, 'ms')
    out('  %-18s старт процесса+модуля med %7.2f ms (min %.2f max %.2f)' % (cand, med(ts), min(ts), max(ts)))
ph = {'compile': [], 'instantiate': [], 'run': []}
for i in range(REPS + 2):
    o, c, ins, ru = Run(T + '/hello.wasm').go()
    if i >= 2: ph['compile'].append(c); ph['instantiate'].append(ins); ph['run'].append(ru)
for k, v in ph.items():
    for i, x in enumerate(v): rec('wasmtime-cranelift', 'startup', k + '_time', i + 1, '%.3f' % x, 'ms')
    out('  wasmtime %-12s med %8.3f ms' % (k, med(v)))
# память wasmtime: дочерний python с загруженным пакетом без модуля против с модулем
base = "import wasmtime;e=wasmtime.Engine();print(1)"
run = "import wasmtime,sys;e=wasmtime.Engine();m=wasmtime.Module.from_file(e,sys.argv[1]);s=wasmtime.Store(e);l=wasmtime.Linker(e);l.define_func('kira','write',wasmtime.FuncType([wasmtime.ValType.i32(),wasmtime.ValType.i32()],[wasmtime.ValType.i32()]),lambda a,b:b);i=l.instantiate(s,m);i.exports(s)['kmain'](s)"
rb = [wait4([sys.executable, '-I', '-c', base])[2] for _ in range(5)]; rr = [wait4([sys.executable, '-I', '-c', run, T + '/sort.wasm'])[2] for _ in range(5)]
out('\nпамять wasmtime: python+wasmtime без модуля peak RSS med %d KiB; с модулем sort (компиляция+выполнение) %d KiB; разница %d KiB' % (med(rb), med(rr), med(rr) - med(rb)))
rec('wasmtime-cranelift', 'sort', 'peak_rss_delta', 1, med(rr) - med(rb), 'KiB', 'дочерний Python: с модулем минус без модуля; медиана 5'); rec('wasmtime-cranelift', 'baseline', 'peak_rss', 1, med(rb), 'KiB', 'Python+пакет wasmtime+Engine')
# размер интерпретатора KIRA
sz = os.path.getsize(os.path.join(os.environ.get('FMT_ART', '/home/claude/kira_os_'), 'userland/wasm.elf'))
out('интерпретатор KIRA userland/wasm.elf: %d Б (ELF, не strip); исходник userland/wasm.c %s строк + wasm_jit_x64.h/gen' % (sz, sum(1 for _ in open(REPO + '/userland/wasm.c'))))
shutil.rmtree(T, ignore_errors=True)
