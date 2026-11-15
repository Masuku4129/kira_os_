#!/usr/bin/env python3
"""Проверка уровней архитектуры (docs/architecture/levels.map).

1. Каждый исходник (.c .h .s .asm) обязан попадать под правило карты.
2. Включение #include "..." допустимо только в сторону того же или нижнего уровня; уровень 0 (сквозной) не ограничен.
3. Уже существующие нарушения записаны в docs/architecture/layer_violations.baseline; новое нарушение - ошибка, а исчезнувшее - тоже ошибка
   (базовый список надо сократить): так число нарушений может только падать.
   python3 ci/check_layers.py            проверить
   python3 ci/check_layers.py --update   перезаписать базовый список (после устранения нарушений)
"""
import fnmatch, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, 'docs/architecture/levels.map')
BASE = os.path.join(ROOT, 'docs/architecture/layer_violations.baseline')

KAPI = []   # заголовки примитивов ядра: драйверы (ур.2) вправе их включать, это интерфейс ядра для нижних уровней

def rules():
    out = []
    for ln in open(MAP, encoding='utf-8'):
        ln = ln.split('#', 1)[0].strip()
        if not ln: continue
        lv, pat = ln.split(None, 1)
        if lv == 'kapi': KAPI.append(pat.strip()); continue
        out.append((int(lv), pat.strip()))
    return out

def main():
    update = '--update' in sys.argv
    files = subprocess.check_output(['git', 'ls-files'], cwd=ROOT, text=True).split('\n')[:-1]
    src = [f for f in files if f.endswith(('.c', '.h', '.s', '.S', '.asm')) and os.path.exists(os.path.join(ROOT, f))]
    rl = rules()
    def level(f):
        for lv, pat in rl:
            if fnmatch.fnmatch(f, pat) or fnmatch.fnmatch(os.path.basename(f), pat) and '/' not in pat:
                return lv
        return None
    lvl = {f: level(f) for f in src}
    errors = [f'не отнесён к уровню: {f}' for f, l in lvl.items() if l is None]
    byb = {}
    for f in src: byb.setdefault(os.path.basename(f), []).append(f)
    viol = set()
    inc = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
    for f in src:
        a = lvl[f]
        if not a: continue
        text = open(os.path.join(ROOT, f), errors='ignore').read()
        for m in inc.finditer(text):
            name = os.path.basename(m.group(1))
            cands = byb.get(name, [])
            if not cands: continue
            same = [c for c in cands if os.path.dirname(c) == os.path.dirname(f)]
            tgt = (same or cands)[0]
            b = lvl.get(tgt)
            if b and a and b > a and not (b == 3 and a == 2 and any(fnmatch.fnmatch(os.path.basename(tgt), k) for k in KAPI)):
                viol.add(f'{f} (ур.{a}) -> {tgt} (ур.{b})')
    base = set(l.strip() for l in open(BASE, encoding='utf-8')) if os.path.exists(BASE) else set()
    base.discard('')
    if update:
        open(BASE, 'w', encoding='utf-8').write('\n'.join(sorted(viol)) + '\n')
        print(f'базовый список: {len(viol)} нарушений'); return 0
    for v in sorted(viol - base): errors.append('НОВОЕ нарушение: ' + v)
    for v in sorted(base - viol): errors.append('нарушение устранено, уберите из базового списка (--update): ' + v)
    for e in errors: print('ОШИБКА', e)
    print(f'файлов {len(src)}, нарушений {len(viol)} (в базе {len(base)})')
    return 1 if errors else 0

sys.exit(main())
