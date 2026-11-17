"""Проверка времени реакции по границам (А8): разбор вывода команды `latency` и сравнение с ci/latency_policy.json.

Вывод ядра (одна строка на вид): `[LAT] irq: n=19174 avg=... p50=... p99=... p99.9=... max=... over=9 limit=10000000 нс, ...`.
Граница задаёт предел метрики (p99, p99.9, max) в нс и минимум замеров, при меньшем числе вид считается не измеренным - это провал
(иначе сценарий, в котором измерения молча не шли, прошёл бы). Используется стендом (ci/kira_vm.py, шаг "latency_policy")."""
import json
import re

LINE = re.compile(r"\[LAT\] ([a-z-]+): n=(\d+)(?: avg=(\d+) p50=(\d+) p99=(\d+) p99\.9=(\d+) max=(\d+))? over=(\d+) limit=(\d+)")
METRICS = ("p50", "p99", "p99.9", "max", "avg")


def parse(text):
    """-> {вид: {"n": .., "avg": .., "p50": .., "p99": .., "p99.9": .., "max": .., "over": .., "limit": ..}}"""
    out = {}
    for m in LINE.finditer(text):
        kind, n, avg, p50, p99, p999, mx, over, limit = m.groups()
        d = {"n": int(n), "over": int(over), "limit": int(limit)}
        if avg is not None:
            d.update({"avg": int(avg), "p50": int(p50), "p99": int(p99), "p99.9": int(p999), "max": int(mx)})
        out[kind] = d
    return out


def load_policy(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def check(measured, policy):
    """Список нарушений (пусто - границы соблюдены). measured - результат parse(), policy - содержимое ci/latency_policy.json."""
    errors = []
    for kind, rule in policy.get("bounds", {}).items():
        m = measured.get(kind)
        if m is None:
            errors.append(f"{kind}: в выводе нет строки [LAT] {kind}")
            continue
        need = rule.get("min_n", 1)
        if need == 0 and m["n"] == 0:
            continue                      # вид, который в норме может не случиться (спин на замке): нет замеров - нет и нарушения
        if m["n"] < need:
            errors.append(f"{kind}: замеров {m['n']}, нужно не меньше {need} (измерение не шло?)")
            continue
        for metric in METRICS:
            if metric in rule:
                if metric not in m:
                    errors.append(f"{kind}: нет значения {metric}")
                elif m[metric] > rule[metric]:
                    errors.append(f"{kind}: {metric} = {m[metric]} нс выше границы {rule[metric]} нс")
    return errors
