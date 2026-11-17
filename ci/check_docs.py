#!/usr/bin/env python3
"""Проверка основных документов: наличие, заполнение, актуальность, ссылки.

Что проверяется:
  1. Обязательные документы существуют и не пустые.
  2. В основных документах есть строка `Обновлено: ГГГГ-ММ-ДД`, не старше MAX_AGE дней.
  3. Каждый docs/**/*.md (и корневые .md) упомянут в docs/INDEX.md; ссылки указателя ведут на существующие файлы.
  4. docs/PLAN.md: у каждого пункта есть поля Зачем/Альтернативы/Критерий готовности/Статус/Зависит от,
     статус из допустимого списка, зависимости ссылаются на существующие пункты.
  5. docs/SYSTEMS.md: у каждой подсистемы шесть полей, пути из «Файлы» существуют, уровень 0..8.
Выход 0 — всё в порядке, 1 — есть замечания. Никаких сетевых вызовов.
"""
import datetime
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_AGE = 45
REQUIRED = ["README.md", "LICENSE", "SECURITY.md", "CONTRIBUTING.md", "docs/PLAN.md", "docs/INDEX.md", "docs/SYSTEMS.md",
            "docs/BACKLOG.md", "docs/QEMU_100.md", "docs/INDUSTRIAL_READINESS.md"]
DATED = ["SECURITY.md", "CONTRIBUTING.md", "README.md", "docs/PLAN.md", "docs/INDEX.md", "docs/SYSTEMS.md", "docs/BACKLOG.md",
         "docs/QEMU_100.md", "docs/INDUSTRIAL_READINESS.md"]
PLAN_FIELDS = ["Зачем", "Альтернативы", "Критерий готовности", "Статус", "Зависит от"]
PLAN_STATUS = {"сделано", "в работе", "не начато", "частично"}
SYS_FIELDS = ["Назначение", "Уровень", "Файлы", "Проверка", "Статус", "Альтернативы"]
SYS_STATUS = ("готово", "частично", "не начато")

errors = []


def err(msg):
    errors.append(msg)


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def fields(block):
    """Поля вида `- Имя: значение` (значение может идти на следующих строках с отступом)."""
    out = {}
    cur = None
    for line in block.splitlines():
        m = re.match(r"^- ([^:]+):\s*(.*)$", line)
        if m:
            cur = m.group(1).strip()
            out[cur] = m.group(2).strip()
        elif cur and line.startswith("  ") and line.strip():
            out[cur] += " " + line.strip()
        elif not line.strip():
            continue
        else:
            cur = None
    return out


def sections(text, pattern):
    """Разбить текст на разделы по заголовку, подходящему под pattern; вернуть [(заголовок, тело)]."""
    parts = re.split(r"(?m)^(%s.*)$" % pattern, text)
    return [(parts[i].strip(), parts[i + 1]) for i in range(1, len(parts) - 1, 2)]


# 1. обязательные
for rel in REQUIRED:
    p = os.path.join(ROOT, rel)
    if not os.path.isfile(p):
        err(f"{rel}: файла нет")
    elif os.path.getsize(p) < 50:
        err(f"{rel}: файл пустой")

# 2. даты
today = datetime.date.today()
for rel in DATED:
    if not os.path.isfile(os.path.join(ROOT, rel)):
        continue
    head = "\n".join(read(rel).splitlines()[:12])
    m = re.search(r"Обновлено:\s*(\d{4})-(\d{2})-(\d{2})", head)
    if not m:
        err(f"{rel}: нет строки «Обновлено: ГГГГ-ММ-ДД» в начале")
        continue
    try:
        d = datetime.date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    except ValueError:
        err(f"{rel}: неверная дата")
        continue
    if (today - d).days > MAX_AGE:
        err(f"{rel}: не обновлялся {(today - d).days} дн. (допустимо {MAX_AGE}); обновите содержимое и дату")
    if d > today + datetime.timedelta(days=1):
        err(f"{rel}: дата в будущем")

# 3. указатель
if os.path.isfile(os.path.join(ROOT, "docs/INDEX.md")):
    idx = read("docs/INDEX.md")
    all_docs = []
    for base, dirs, files in os.walk(os.path.join(ROOT, "docs")):
        for fn in files:
            if fn.endswith(".md"):
                all_docs.append(os.path.relpath(os.path.join(base, fn), os.path.join(ROOT, "docs")))
    for rel in sorted(all_docs):
        if rel not in idx:
            err(f"docs/{rel}: нет в docs/INDEX.md")
    for name in ("README.md", "AGENTS.md", "ARCHITECTURE.md", "LIMBO.md", "LIMBO_ARM.md", "LICENSE", "SECURITY.md", "CONTRIBUTING.md"):
        if os.path.isfile(os.path.join(ROOT, name)) and ("../" + name) not in idx:
            err(f"{name}: нет в docs/INDEX.md")
    for target in re.findall(r"\]\(([^)#]+)\)", idx):
        if target.startswith("http"):
            continue
        if not os.path.exists(os.path.normpath(os.path.join(ROOT, "docs", target))):
            err(f"docs/INDEX.md: ссылка на несуществующий файл {target}")

# 4. план
if os.path.isfile(os.path.join(ROOT, "docs/PLAN.md")):
    plan = read("docs/PLAN.md")
    items = sections(plan, r"### ")
    ids = set()
    for title, _ in items:
        m = re.match(r"### ([А-ЯA-Z]\d+)\.", title)
        if m:
            ids.add(m.group(1))
        else:
            err(f"docs/PLAN.md: заголовок пункта без номера: {title}")
    if not items:
        err("docs/PLAN.md: нет пунктов")
    for title, body in items:
        f = fields(body)
        for name in PLAN_FIELDS:
            if not f.get(name):
                err(f"docs/PLAN.md [{title}]: не заполнено поле «{name}»")
        st = f.get("Статус", "")
        if st and st not in PLAN_STATUS:
            err(f"docs/PLAN.md [{title}]: статус «{st}» не из {sorted(PLAN_STATUS)}")
        dep = f.get("Зависит от", "")
        if dep and dep != "нет":
            for d in re.findall(r"[А-ЯA-Z]\d+", dep):
                if d not in ids:
                    err(f"docs/PLAN.md [{title}]: зависимость от несуществующего пункта {d}")
            if not re.findall(r"[А-ЯA-Z]\d+|этап", dep):
                err(f"docs/PLAN.md [{title}]: непонятная зависимость «{dep}»")

# 5. подсистемы
if os.path.isfile(os.path.join(ROOT, "docs/SYSTEMS.md")):
    systems = read("docs/SYSTEMS.md")
    subs = sections(systems, r"## Подсистема: ")
    if len(subs) < 10:
        err(f"docs/SYSTEMS.md: подсистем {len(subs)}, ожидается не меньше 10")
    seen = set()
    for title, body in subs:
        if title in seen:
            err(f"docs/SYSTEMS.md: повтор подсистемы {title}")
        seen.add(title)
        f = fields(body)
        for name in SYS_FIELDS:
            if not f.get(name):
                err(f"docs/SYSTEMS.md [{title}]: не заполнено поле «{name}»")
        lv = f.get("Уровень", "")
        m = re.match(r"(\d)", lv)
        if lv and (not m or int(m.group(1)) > 8):
            err(f"docs/SYSTEMS.md [{title}]: уровень «{lv}» вне 0..8")
        st = f.get("Статус", "")
        if st and not st.startswith(SYS_STATUS):
            err(f"docs/SYSTEMS.md [{title}]: статус должен начинаться с одного из {SYS_STATUS}")
        for path in re.findall(r"`([^`]+)`", f.get("Файлы", "")):
            if not os.path.exists(os.path.join(ROOT, path)):
                err(f"docs/SYSTEMS.md [{title}]: файла нет: {path}")

if errors:
    print(f"check_docs: замечаний {len(errors)}")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print("check_docs: OK")
