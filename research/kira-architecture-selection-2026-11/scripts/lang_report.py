#!/usr/bin/env python3 -I
"""Сводные таблицы из data/results_lang.csv (медиана [мин..макс], геомеан относительно эталона; COMPACT=1 - только медиана). Использование: lang_report.py EXP метрика эталон [кандидаты...] | lang_report.py passes. Время в таблицах - мс."""
import csv, sys, statistics as st, math, collections, os
csvp = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "results_lang.csv")
def load(exp):
    d = collections.defaultdict(list)
    for r in csv.DictReader(open(csvp, encoding="utf-8")):
        if r["exp"] == exp and r["value"] != "":
            try: d[(r["candidate"], r["workload"], r["metric"])].append(float(r["value"]))
            except ValueError: pass
    return d
def table(exp, metric, ref, cands=None, fmt="{:.0f}", scale=1.0, rep_filter=True):
    d = load(exp)
    cs = cands or sorted({k[0] for k in d if k[2] == metric})
    wls = []
    for k in d:
        if k[2] == metric and k[1] not in wls: wls.append(k[1])
    print("| нагрузка | " + " | ".join(cs) + " |"); print("|---|" + "---|" * len(cs))
    logs = {c: [] for c in cs}
    for w in wls:
        row = []
        rv = d.get((ref, w, metric))
        for c in cs:
            v = d.get((c, w, metric))
            if not v: row.append("n/a"); continue
            m = st.median(v) * scale
            cell = fmt.format(m) + ("" if (len(v) == 1 or os.environ.get("COMPACT")) else " [{}..{}]".format(fmt.format(min(v) * scale), fmt.format(max(v) * scale)))
            if rv and c != ref: cell += " ×{:.2f}".format(st.median(v) / st.median(rv)); logs[c].append(math.log(st.median(v) / st.median(rv)))
            row.append(cell)
        print("| " + w + " | " + " | ".join(row) + " |")
    print("| **геомеан к " + ref + "** | " + " | ".join(("1.00" if c == ref else ("×{:.3f} (n={})".format(math.exp(sum(logs[c]) / len(logs[c])), len(logs[c])) if logs[c] else "n/a")) for c in cs) + " |")
if __name__ == "__main__" and sys.argv[1] != "passes":
    exp, metric, ref = sys.argv[1:4]
    table(exp, metric, ref, sys.argv[4:] or None, scale=0.001 if metric.endswith("_us") else 1.0, fmt="{:.1f}" if metric.endswith("_us") else "{:.0f}")

def passes():
    """L8: проход -> эффект. Отношение медиан (вариант без прохода / парная база того же прогона): геомеан по нагрузкам, худшая и лучшая нагрузка; то же по размеру .text.
    Парная база kcc-base@<флаг> измерялась вперемежку с вариантом (дрейф скорости машины сокращается)."""
    d = load("L8")
    cands = sorted({k[0] for k in d if k[0].startswith("kcc-f") or k[0] == "kcc-O0"})
    wls = sorted({k[1] for k in d if k[2] == "run_us"})
    print("| проход (отключён флагом) | геомеан времени | худшая нагрузка | лучшая нагрузка | геомеан .text | нагрузок (n) |"); print("|---|---|---|---|---|---|")
    rows = []
    for c in cands:
        base = "kcc-base@" + c[len("kcc"):]
        rr, tt, nm = [], [], []
        for w in wls:
            a, b = d.get((c, w, "run_us")), d.get((base, w, "run_us"))
            if a and b: rr.append(st.median(a) / st.median(b)); nm.append((rr[-1], w))
            ta, tb = d.get((c, w, "text_bytes")), d.get((base, w, "text_bytes"))
            if ta and tb: tt.append(ta[0] / tb[0])
        if rr: rows.append((math.exp(sum(math.log(x) for x in rr) / len(rr)), c, max(nm), min(nm), math.exp(sum(math.log(x) for x in tt) / len(tt)) if tt else float("nan"), len(rr)))
    for g, c, mx, mn, tg, n in sorted(rows, reverse=True):
        print("| {} | ×{:.3f} | {} ×{:.2f} | {} ×{:.2f} | ×{:.3f} | {} |".format(c, g, mx[1], mx[0], mn[1], mn[0], tg, n))
if len(sys.argv) > 1 and sys.argv[1] == "passes": passes()
