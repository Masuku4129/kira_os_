#!/usr/bin/env python3 -I
"""[E] Время загрузки KIRA в QEMU (TCG, без KVM) до маркера «Command line ready» в serial. Wall-time от запуска процесса QEMU.
Запуск: taskset -c 0 python3 -I scripts/fmt_boot.py bios|uefi|arm64 REPS [timeout_s]. Образы - ТОЛЬКО ЧТЕНИЕ (-snapshot); VARS OVMF - копия на каждый запуск.
Пишет: logs/fmt_boot_<mode>_<i>.log (serial), results_fmt.csv (t_marker, t_first_byte, t_kernel_start), logs/fmt_boot_<mode>.summary.
Время в TCG - справка о среде, не оценка железа и не оценка скорости ядра."""
import os, subprocess, sys, time, shutil, tempfile, statistics
ART = os.environ.get('FMT_ART', '/home/claude/kira_os_')
HERE = os.path.dirname(os.path.abspath(__file__)); LOGS = os.path.join(HERE, '..', 'logs'); CSV = os.path.join(HERE, '..', 'data', 'results_fmt.csv')
mode = sys.argv[1]; reps = int(sys.argv[2]); tmo = float(sys.argv[3]) if len(sys.argv) > 3 else 90
MARK = b'Command line ready'; MARKS = [('first_byte', None), ('kernel_start', b'Kernel core starting'), ('marker', MARK)]
tmp = tempfile.mkdtemp(prefix='fmtboot', dir=os.environ.get('TMPDIR'))
def cmd(i, serial):
    if mode == 'bios':
        return ['qemu-system-x86_64', '-drive', 'file=%s/kira-bios.img,format=raw,snapshot=on' % ART, '-m', '256', '-display', 'none', '-serial', 'file:' + serial, '-no-reboot']
    if mode == 'uefi':
        v = os.path.join(tmp, 'vars%d.fd' % i); shutil.copy('/usr/share/OVMF/OVMF_VARS_4M.fd', v)
        return ['qemu-system-x86_64', '-machine', 'q35', '-m', '256', '-display', 'none', '-serial', 'file:' + serial,
                '-drive', 'if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd', '-drive', 'if=pflash,format=raw,file=' + v,
                '-drive', 'file=%s/kira-uefi.img,format=raw,if=virtio,snapshot=on' % ART, '-no-reboot']
    if mode == 'arm64':
        d = os.path.join(tmp, 'a64.disk%d' % i); shutil.copy(ART + '/kira-arm64.disk', d)
        return ['qemu-system-aarch64', '-M', 'virt,gic-version=3', '-cpu', 'cortex-a72', '-smp', '4', '-m', '1G', '-display', 'none', '-serial', 'file:' + serial, '-kernel', ART + '/kira-arm64.img',
                '-drive', 'file=%s,if=none,id=d0,format=raw' % d, '-device', 'virtio-blk-pci,drive=d0', '-no-reboot']
    sys.exit('mode')
res = {k: [] for k, _ in MARKS}; fails = 0
for i in range(1, reps + 1):
    serial = os.path.join(LOGS, 'fmt_boot_%s_%d.log' % (mode, i)); open(serial, 'wb').close()
    t0 = time.perf_counter(); p = subprocess.Popen(cmd(i, serial), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    seen = {}; data = b''
    while time.perf_counter() - t0 < tmo:
        data = open(serial, 'rb').read()
        now = time.perf_counter() - t0
        if data and 'first_byte' not in seen: seen['first_byte'] = now
        for k, m in MARKS:
            if m and k not in seen and m in data: seen[k] = now
        if 'marker' in seen or p.poll() is not None: break
        time.sleep(0.005)
    p.terminate()
    try: p.wait(5)
    except Exception: p.kill(); p.wait()
    data = open(serial, 'rb').read()
    ok = 'marker' in seen
    print('%s run %d: %s  first_byte=%s kernel_start=%s marker=%s (serial %d Б)' % (mode, i, 'OK' if ok else 'FAIL', *(('%.3f' % seen[k]) if k in seen else '-' for k in ('first_byte', 'kernel_start', 'marker')), len(data)))
    if not ok: fails += 1
    for k, _ in MARKS:
        if k in seen:
            res[k].append(seen[k] * 1000)
            open(CSV, 'a').write('fmt_boot,%s,vm-core0,%s,boot,t_%s,%d,%.1f,ms,%d,"QEMU TCG без KVM, wall от Popen, опрос 5 мс; образ -snapshot"\n' % ('aarch64' if mode == 'arm64' else 'x86_64', mode, k, i, seen[k] * 1000, 0 if ok else 1))
    if not ok: open(CSV, 'a').write('fmt_boot,%s,vm-core0,%s,boot,marker_missing,%d,1,flag,1,"маркер не получен за %ds"\n' % ('aarch64' if mode == 'arm64' else 'x86_64', mode, i, tmo))
with open(os.path.join(LOGS, 'fmt_boot_%s.summary' % mode), 'w') as f:
    for k, v in res.items():
        if v: f.write('%s %s: n=%d med %.0f ms min %.0f max %.0f\n' % (mode, k, len(v), statistics.median(v), min(v), max(v)))
    f.write('%s: запусков %d, без маркера %d\n' % (mode, reps, fails))
print(open(os.path.join(LOGS, 'fmt_boot_%s.summary' % mode)).read())
shutil.rmtree(tmp, ignore_errors=True)
