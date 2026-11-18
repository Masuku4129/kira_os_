#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kira-Proprietary
"""Проверка лицензионной инвентаризации (docs/THIRD_PARTY.md). Это не юридическая проверка, а защита от забывчивости.

Правила:
  1. Каждый файл исходников (.c .h .s .S .asm .py .sh .rs .ld .inc) в git имеет в первых 25 строках SPDX-License-Identifier
     либо перечислен в ci/license_baseline.txt (явный список «заголовка нет, файл наш»). Устаревшая запись списка - ошибка.
     Новый наш файл: добавьте первой строкой SPDX-License-Identifier: LicenseRef-Kira-Proprietary.
  2. Файл с чужим SPDX (не LicenseRef-Kira-Proprietary) или чужой строкой «Copyright (c) ГГГГ ...» в первых 30 строках
     должен попадать под запись блока files в docs/THIRD_PARTY.md.
  3. Каждый отслеженный файл, не являющийся исходником и не .md (бинарники, данные, образы, конфиги), попадает под запись блока files.
  4. Каждая запись files ссылается на компонент из блока components; у записи есть хотя бы один файл (иначе устарела).
  5. Компоненты: допустимые вывод и область; при notice=yes имя компонента есть в NOTICE.
  6. Закреплённые коммиты (tools/mpy_kira/pins.txt, drivers/bsdcorpus/manifest.txt) упомянуты в docs/THIRD_PARTY.md.
Выход 0 - замечаний нет. Сеть не используется."""
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_EXT = {".c", ".h", ".s", ".S", ".asm", ".py", ".sh", ".rs", ".ld", ".inc"}
KIRA_ID = "LicenseRef-Kira-Proprietary"
VERDICTS = {"можно", "можно с условиями", "риск", "нельзя"}
SCOPES = {"образ", "сборка", "тесты", "архив", "не поставляется"}
SPDX_RE = re.compile(r"^\s*(?:/\*|//|#|\*)\s*SPDX-License-Identifier:\s*([A-Za-z0-9.+-]+)", re.M)
COPY_RE = re.compile(r"Copyright\s*(?:\(c\)|\(C\)|©)\s*\d{4}", re.I)
OURS_RE = re.compile(r"Kira|Пастушенко|Pastushenko", re.I)


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files"], check=True, capture_output=True, text=True).stdout
    return [p for p in out.split("\n") if p and os.path.isfile(os.path.join(ROOT, p))]


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def block(doc, name):
    m = re.search(r"```text\n# licenses:" + name + r"\n(.*?)```", doc, re.S)
    rows = []
    if m:
        for ln in m.group(1).split("\n"):
            ln = ln.strip()
            if ln and not ln.startswith("#"):
                rows.append([c.strip() for c in ln.split("|")])
    return rows


def head(rel, n):
    try:
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            return "".join(f.readline() for _ in range(n))
    except OSError:
        return ""


def check(files=None, doc=None, baseline=None, notice=None, pins=None, extra_heads=None):
    """Возвращает список замечаний. Параметры позволяют подменять входы (самопроверка ci/test_check_licenses.py)."""
    errs = []
    files = tracked() if files is None else files
    doc = read("docs/THIRD_PARTY.md") if doc is None else doc
    baseline = set(l.strip() for l in read("ci/license_baseline.txt").split("\n") if l.strip() and not l.startswith("#")) \
        if baseline is None else set(baseline)
    notice = read("NOTICE") if notice is None else notice
    extra_heads = extra_heads or {}
    comps = block(doc, "components")
    frows = block(doc, "files")
    cids = set()
    for r in comps:
        if len(r) != 5:
            errs.append("components: ожидается 5 полей id | SPDX | область | вывод | notice: %r" % r)
            continue
        cid, spdx, scope, verdict, nt = r
        cids.add(cid)
        if verdict not in VERDICTS:
            errs.append("components %s: вывод %r вне %s" % (cid, verdict, sorted(VERDICTS)))
        if scope not in SCOPES:
            errs.append("components %s: область %r вне %s" % (cid, scope, sorted(SCOPES)))
        if nt not in ("yes", "no"):
            errs.append("components %s: notice должно быть yes/no" % cid)
        if nt == "yes" and cid.lower() not in notice.lower():
            errs.append("components %s: notice=yes, но имени нет в NOTICE" % cid)
        if doc.count(cid) < 2:
            errs.append("components %s: компонент не описан в тексте документа (нужен раздел с этим именем)" % cid)
    globs = []
    for r in frows:
        if len(r) != 4:
            errs.append("files: ожидается 4 поля glob | класс | SPDX | компонент: %r" % r)
            continue
        g, cls, spdx, cid = r
        if cls not in ("own", "third", "risk"):
            errs.append("files %s: класс %r вне own/third/risk" % (g, cls))
        if cid not in cids:
            errs.append("files %s: компонент %r не описан в блоке components" % (g, cid))
        globs.append(g)
    used = set()

    def listed(p):
        hit = [g for g in globs if fnmatch.fnmatchcase(p, g)]
        used.update(hit)
        return bool(hit)

    seen_base = set()
    for p in sorted(files):
        ext = os.path.splitext(p)[1]
        listed(p)
        if ext in SRC_EXT:
            h = extra_heads.get(p)
            h = head(p, 30) if h is None else h
            m = SPDX_RE.search("\n".join(h.split("\n")[:25]))
            if m:
                if m.group(1) != KIRA_ID and not listed(p):
                    errs.append("%s: SPDX %s не наш и файла нет в блоке files docs/THIRD_PARTY.md" % (p, m.group(1)))
                if p in baseline:
                    errs.append("%s: в ci/license_baseline.txt, но SPDX уже есть - уберите из списка" % p)
                    seen_base.add(p)
            else:
                if p not in baseline:
                    errs.append("%s: нет SPDX-License-Identifier и нет в ci/license_baseline.txt" % p)
                else:
                    seen_base.add(p)
            if any(COPY_RE.search(ln) and not OURS_RE.search(ln) for ln in h.split("\n")) and not listed(p):
                errs.append("%s: чужая строка Copyright и файла нет в блоке files docs/THIRD_PARTY.md" % p)
        elif ext != ".md":
            if not listed(p):
                errs.append("%s: файл данных/бинарник не перечислен в блоке files docs/THIRD_PARTY.md" % p)
    for p in sorted(baseline - seen_base):
        errs.append("ci/license_baseline.txt: %s - такого файла исходников нет (устарело)" % p)
    for g in globs:
        if g not in used:
            errs.append("files: запись %r не подошла ни к одному файлу (устарела)" % g)
    # закреплённые коммиты
    if pins is None:
        pins = []
        for rel, rx in (("tools/mpy_kira/pins.txt", r"^[A-Z_]+=([0-9a-f]{40})\s*$"), ("drivers/bsdcorpus/manifest.txt", r"^commit ([0-9a-f]{40})\s*$")):
            if os.path.isfile(os.path.join(ROOT, rel)):
                pins += re.findall(rx, read(rel), re.M)
    for v in pins:
        if v not in doc:
            errs.append("закреплённый коммит %s не упомянут в docs/THIRD_PARTY.md" % v)
    return errs


def main():
    errs = check()
    for e in errs:
        print("check_licenses: " + e)
    if errs:
        print("check_licenses: замечаний %d" % len(errs))
        return 1
    print("check_licenses: ок")
    return 0


if __name__ == "__main__":
    sys.exit(main())
