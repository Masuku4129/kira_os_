#!/usr/bin/env python3
"""Корпус исходников OpenBSD для слоя совместимости BSD (BSD-лицензия, чужой код в репозиторий не кладётся).
Файлы скачиваются по закреплённому коммиту, SHA-256 каждого сверяется с drivers/bsdcorpus/manifest.txt.
  tools/bsd_corpus.py fetch [DIR]     скачать и проверить (по умолчанию build-bsd/corpus)
  tools/bsd_corpus.py pin [КОММИТ]    пересчитать хеши по коммиту и переписать manifest.txt (для обновления корпуса)
Формат manifest.txt:  commit <sha1>  /  set <имя> <описание>  /  file <набор> <путь относительно sys/> <sha256>"""
import hashlib, os, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, 'drivers', 'bsdcorpus', 'manifest.txt')
RAW = 'https://raw.githubusercontent.com/openbsd/src/%s/sys/%s'

def read_manifest():
    commit, sets, files = None, {}, []
    for ln in open(MANIFEST, encoding='utf-8'):
        p = ln.split()
        if not p or p[0].startswith('#'): continue
        if p[0] == 'commit': commit = p[1]
        elif p[0] == 'set': sets[p[1]] = ' '.join(p[2:])
        elif p[0] == 'file': files.append((p[1], p[2], p[3] if len(p) > 3 else ''))
    return commit, sets, files

def get(commit, path):
    with urllib.request.urlopen(RAW % (commit, path), timeout=60) as r: return r.read()

def fetch(dest):
    commit, sets, files = read_manifest(); bad = 0
    for s, path, sha in files:
        out = os.path.join(dest, s, 'sys', path)
        if os.path.exists(out) and hashlib.sha256(open(out, 'rb').read()).hexdigest() == sha: continue
        data = get(commit, path)
        got = hashlib.sha256(data).hexdigest()
        if got != sha: print('ХЕШ НЕ СОВПАЛ', path, got); bad += 1; continue
        os.makedirs(os.path.dirname(out), exist_ok=True); open(out, 'wb').write(data)
    print('корпус: %d файлов, коммит %s, ошибок %d' % (len(files), commit[:10], bad)); return 1 if bad else 0

def pin(commit):
    old_commit, sets, files = read_manifest()
    lines = ['# Корпус OpenBSD (sys/): BSD-лицензия. Хеши сверяет tools/bsd_corpus.py fetch.', 'commit ' + commit]
    for k, v in sets.items(): lines.append('set %s %s' % (k, v))
    for s, path, _ in files: lines.append('file %s %s %s' % (s, path, hashlib.sha256(get(commit, path)).hexdigest()))
    open(MANIFEST, 'w', encoding='utf-8').write('\n'.join(lines) + '\n'); print('закреплено:', commit)

if __name__ == '__main__':
    a = sys.argv[1:] or ['fetch']
    if a[0] == 'fetch': sys.exit(fetch(a[1] if len(a) > 1 else os.path.join(ROOT, 'build-bsd', 'corpus')))
    elif a[0] == 'pin': pin(a[1] if len(a) > 1 else read_manifest()[0])
    else: print(__doc__); sys.exit(2)
