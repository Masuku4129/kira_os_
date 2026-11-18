#!/usr/bin/env python3
"""Проверка плавности перетаскивания окон по строкам [PERIOD] и [WMPERF] команды fb (docs/WM_PERF.md).

Берутся периоды, где мышь двигалась (mouse_used >= drag_min_mouse), и сверяются с порогами из ci/wmperf_policy.json.
  python3 ci/wmperf_check.py ПРОФИЛЬ ЖУРНАЛ       проверить журнал (код возврата 1 - порог нарушен)
  python3 ci/wmperf_check.py --selftest           проверка самой проверки: замеры ДО исправлений обязаны нарушить пороги, замеры ПОСЛЕ - нет
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load_policy(profile):
    return json.load(open(os.path.join(HERE, "wmperf_policy.json"), encoding="utf-8"))[profile]


def samples(text, drag_min_mouse):
    out = []
    for m in re.finditer(r"\[PERIOD\] (.*)\n(?:.*\n)*?\s*\[WMPERF\] (.*)", text):
        per = {k: int(v) for k, v in re.findall(r"(\w+) (\d+)", m.group(1))}
        wp = {k: int(v) for k, v in re.findall(r"(\w+) (\d+)", m.group(2))}
        if per.get("mouse_used", 0) < drag_min_mouse:
            continue
        frames = max(1, per.get("frames", 1))
        wp["late_pct"] = 100 * per.get("late", 0) // frames
        wp["fps"] = frames * 1000 // max(1, per.get("ms", 1))
        wp["content_over_px_x100"] = 100 * wp.get("content_px", 0) // max(1, wp.get("px_per_frame", 1))
        out.append(wp)
    return out


def check(pol, text, min_samples=None):
    """Вернуть (замеры, список нарушений)."""
    sm = samples(text, pol.get("drag_min_mouse", 800))
    need = pol.get("min_samples", 2) if min_samples is None else min_samples
    bad = []
    if len(sm) < need:
        bad.append(f"найдено {len(sm)} перетаскиваний, нужно {need}")
    for i, wp in enumerate(sm):
        for key, lim in pol.get("max", {}).items():
            if wp.get(key, 0) > lim:
                bad.append(f"перетаскивание {i}: {key} = {wp.get(key, 0)} > {lim}")
        for key, lim in pol.get("min", {}).items():
            if wp.get(key, 0) < lim:
                bad.append(f"перетаскивание {i}: {key} = {wp.get(key, 0)} < {lim}")
    return sm, bad


def selftest():
    fx = os.path.join(ROOT, "tests/fixtures/wmperf")
    rc = 0
    for profile, before, after in (("virtio1412", "before_virtio.log", "after_virtio.log"), ("vga1280", "before_small.log", "after_small.log")):
        pol = load_policy(profile)
        sb, bb = check(pol, open(os.path.join(fx, before), encoding="utf-8").read(), min_samples=1)
        sa, ba = check(pol, open(os.path.join(fx, after), encoding="utf-8").read(), min_samples=1)
        print(f"wmperf {profile}: до исправлений - {len(bb)} нарушений (ожидается > 0), после - {len(ba)} (ожидается 0)")
        if not bb:
            print("  ОШИБКА: замеры до исправлений не нарушают пороги - проверка не поймает регрессию"); rc = 1
        if ba:
            print("  ОШИБКА: замеры после исправлений нарушают пороги:", "; ".join(ba)); rc = 1
    if rc == 0:
        print("wmperf_check: OK")
    return rc


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        sys.exit(selftest())
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(2)
    sm, bad = check(load_policy(sys.argv[1]), open(sys.argv[2], encoding="utf-8", errors="replace").read())
    print(json.dumps(sm, ensure_ascii=False))
    for b in bad:
        print("НАРУШЕНИЕ:", b)
    sys.exit(1 if bad else 0)
