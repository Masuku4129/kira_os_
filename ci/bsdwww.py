"""Тестовый «хозяин» драйверов BSD для сценария drvget-bsd: рецепт и сгенерированные файлы (tools/bsd_recipe.py) + зеркало файлов корпуса OpenBSD
под теми же путями и теми же SHA-256, что в рецепте. Возвращает (каталог, первые 8 знаков хеша набора) или None, если корпус недоступен."""
import hashlib, os, shutil, struct, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

def build(name="ne_pci", cset="ne2k", mirror="https://10.0.2.100/bsd"):
    if subprocess.run([sys.executable, os.path.join(ROOT, "tools/bsd_corpus.py"), "fetch"], capture_output=True).returncode: return None
    commit = [l.split()[1] for l in open(os.path.join(ROOT, "drivers/bsdcorpus/manifest.txt")) if l.startswith("commit ")][0]
    sysd = os.path.join(ROOT, "build-bsd/corpus", cset, "sys")
    w = os.path.abspath("ci-out/bsdwww"); shutil.rmtree(w, ignore_errors=True); os.makedirs(w)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/bsd_recipe.py"), sysd, os.path.join(ROOT, "drivers/bsdcorpus/index.txt"),
                        os.path.join(ROOT, "build-bsd/corpus/pcidevs/sys/dev/pci/pcidevs"), name, commit, w, "--corpus-base", mirror], capture_output=True, text=True)
    if r.returncode: sys.stderr.write(r.stderr); return None
    dn = os.listdir(w)[0]; d = os.path.join(w, dn)
    rec = open(os.path.join(d, "drvget.rec")).read().splitlines()
    names = ["drvget.rec"]
    for ln in rec:
        p = ln.split()
        if p and p[0] == "file":
            names.append(p[1])
            if p[2].startswith(mirror):                                   # файл корпуса: зеркало под тем же путём
                rel = p[2][len(mirror) + 1:]; dst = os.path.join(w, "bsd", rel); os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy(os.path.join(sysd, rel), dst)
    h = hashlib.sha256()
    for n in names:                                                       # как dg_set_add в userland/drvget_core.c
        data = open(os.path.join(d, n) if n in os.listdir(d) else os.path.join(w, "bsd", [p[2] for p in (l.split() for l in rec) if p and p[0] == "file" and p[1] == n][0][len(mirror) + 1:]), "rb").read()
        h.update(n.encode() + b"\0" + struct.pack("<Q", len(data)) + data)
    return w, dn, h.hexdigest()[:8]
