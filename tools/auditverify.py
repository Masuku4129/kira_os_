#!/usr/bin/env python3
"""Независимая проверка журнала аудита Kira OS (docs/AUDIT.md): без ядра, по открытому ключу устройства.

Использование:
  tools/auditverify.py --pub КЛЮЧ [--ckpt audit.ckpt] audit.0000000001.log ... audit.log
  tools/auditverify.py --pub КЛЮЧ --export страницы.txt        (строки AUDREC из команды `audit export`)
КЛЮЧ - 64 шестнадцатеричных знака (команда `audit key` на устройстве) или файл с ними. Файлы сегментов - в порядке возрастания номеров,
последним - текущий /var/audit.log (если он дублирует начало последнего закрытого, дубликат пропускается так же, как в ядре).
Код возврата: 0 - журнал цел, 1 - нарушение, 2 - ошибка использования. Формат записи и правила - те же, что в auditcore.c (160 байт, SHA-256, Ed25519)."""
import hashlib, struct, sys

REC, HDR, SEG = 160, 256, 256
K_CKPT, K_PRUNE = 2, 4
KINDS = ["?", "boot", "ckpt", "loss", "prune", "key", "exec", "cap-deny", "drv-install", "drv-remove", "rcon-passwd", "rcon-login", "mount",
         "config", "trust", "sleep", "wake", "panic", "watchdog", "immune", "time", "shutdown", "tamper", "rcon-cmd"]
SRCS = ["kernel", "console", "rcon", "program", "system"]
RES = ["OK", "DENIED", "FAIL"]
DOMAIN = b"KIRA-AUDIT-CKPT1"
WHY = {"FORMAT": "формат файла нарушен (обрезан или чужой)", "KEY": "ключ не принадлежит устройству",
       "ANCHOR": "начало сегмента не подтверждено подписью или не стыкуется", "SEQ": "разрыв, повтор или перестановка номеров (запись удалена, вставлена или переставлена)",
       "CHAIN": "цепочка хэшей разорвана (запись изменена, подменена или удалена)", "SIG": "подпись контрольной точки неверна",
       "TRUNC": "журнал короче подтверждённого подписью (хвост отрезан)", "PRUNE": "начало журнала удалено без записи prune"}

# ---- Ed25519: python cryptography, а без неё - справочная реализация RFC 8032 (медленно, но самодостаточно) ----
try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    def ed_verify(pub, msg, sig):
        try:
            Ed25519PublicKey.from_public_bytes(pub).verify(sig, msg); return True
        except (InvalidSignature, ValueError):
            return False
except ImportError:
    _p = 2**255 - 19
    _L = 2**252 + 27742317777372353535851937790883648493
    _d = -121665 * pow(121666, _p - 2, _p) % _p
    _I = pow(2, (_p - 1) // 4, _p)
    def _recover(y, sign):
        x2 = (y * y - 1) * pow(_d * y * y + 1, _p - 2, _p) % _p
        x = pow(x2, (_p + 3) // 8, _p)
        if (x * x - x2) % _p: x = x * _I % _p
        if (x * x - x2) % _p: return None
        if x & 1 != sign: x = _p - x
        return x
    def _add(P, Q):
        x1, y1, z1, t1 = P; x2, y2, z2, t2 = Q
        a = (y1 - x1) * (y2 - x2) % _p; b = (y1 + x1) * (y2 + x2) % _p
        c = 2 * t1 * t2 * _d % _p; dd = 2 * z1 * z2 % _p
        e, f, g, h = b - a, dd - c, dd + c, b + a
        return (e * f % _p, g * h % _p, f * g % _p, e * h % _p)
    def _mul(s, P):
        Q = (0, 1, 1, 0)
        while s:
            if s & 1: Q = _add(Q, P)
            P = _add(P, P); s >>= 1
        return Q
    def _dec(b):
        y = int.from_bytes(b, "little"); sign = y >> 255; y &= (1 << 255) - 1
        if y >= _p: return None
        x = _recover(y, sign)
        return None if x is None else (x, y, 1, x * y % _p)
    _By = 4 * pow(5, _p - 2, _p) % _p
    _B = (_recover(_By, 0), _By, 1, _recover(_By, 0) * _By % _p)
    def _eq(P, Q): return (P[0] * Q[2] - Q[0] * P[2]) % _p == 0 and (P[1] * Q[2] - Q[1] * P[2]) % _p == 0
    def ed_verify(pub, msg, sig):
        A = _dec(pub); R = _dec(sig[:32])
        if A is None or R is None: return False
        s = int.from_bytes(sig[32:], "little")
        if s >= _L: return False
        k = int.from_bytes(hashlib.sha512(sig[:32] + pub + msg).digest(), "little") % _L
        return _eq(_mul(s, _B), _add(R, _mul(k, A)))


def rec_hash(r):
    r = bytearray(r)
    if struct.unpack_from("<H", r, 24)[0] == K_CKPT: r[40:104] = bytes(64)
    return hashlib.sha256(bytes(r)).digest()

def ckpt_ok(r, pub):
    if struct.unpack_from("<H", r, 24)[0] != K_CKPT: return False
    if hashlib.sha256(pub).digest()[:16] != r[104:120]: return False
    return ed_verify(pub, DOMAIN + rec_hash(r), bytes(r[40:104]))

def parse(r):
    seq, mono, real, kind, src, res, pid, dom, arg = struct.unpack_from("<QQQHBBIII", r, 0)
    subj = bytes(r[40:64]).split(b"\0")[0].decode("utf-8", "replace")
    det = bytes(r[64:120]).split(b"\0")[0].decode("utf-8", "replace")
    boot = struct.unpack_from("<I", r, 120)[0]
    return dict(seq=seq, mono=mono, real=real, kind=kind, src=src, res=res, pid=pid, dom=dom, arg=arg, subj=subj, det=det, boot=boot)

class V:
    def __init__(self, pub):
        self.pub = pub; self.started = False; self.next = 0; self.head = bytes(32); self.first = 0
        self.count = 0; self.ckpts = 0; self.prune_upto = 0; self.bad = []; self.files = 0
        self.want = None; self.want_seen = False; self.records = []
    def fail(self, code, seq, off=0):
        self.bad.append((code, seq, self.files - 1, off))

    def add_file(self, name, b):
        self.files += 1
        if len(b) < HDR or b[:8] != b"KIRAAUD1": return self.fail("FORMAT", self.next)
        ver, rs, first = struct.unpack_from("<IIQ", b, 8)
        if ver != 1 or rs != REC or first == 0: return self.fail("FORMAT", self.next)
        hpub, anchor, actk = b[24:56], b[56:88], b[96:256]
        if hpub != self.pub: self.fail("KEY", first)
        body = len(b) - HDR; n = body // REC
        if body % REC: self.fail("FORMAT", first + n, HDR + n * REC)
        if not self.started:
            self.started = True; self.first = first; self.next = first
            if first == 1:
                if any(anchor) or any(actk): self.fail("ANCHOR", 1)
                self.head = bytes(32)
            else:
                a_seq = struct.unpack_from("<Q", actk, 0)[0]
                if not (struct.unpack_from("<H", actk, 24)[0] == K_CKPT and a_seq + 1 == first and rec_hash(actk) == anchor and ckpt_ok(actk, self.pub)):
                    self.fail("ANCHOR", first)
                self.head = anchor
        elif first != self.next or anchor != self.head:
            self.fail("SEQ" if first != self.next else "ANCHOR", first)
            self.next = first; self.head = anchor
        for i in range(n):
            off = HDR + i * REC; r = b[off:off + REC]; p = parse(r)
            if p["seq"] != self.next: self.fail("SEQ", self.next, off); self.next = p["seq"]
            elif r[128:160] != self.head: self.fail("CHAIN", p["seq"], off)
            if p["seq"] % SEG == 0 and p["kind"] != K_CKPT: self.fail("SEQ", p["seq"], off)
            if p["kind"] == K_CKPT:
                if not ckpt_ok(r, self.pub): self.fail("SIG", p["seq"], off)
                else: self.ckpts += 1
            elif p["kind"] == K_PRUNE: self.prune_upto = max(self.prune_upto, p["arg"])
            self.head = rec_hash(r)
            if self.want and p["seq"] == self.want[0]:
                self.want_seen = True
                if self.head != self.want[1]: self.fail("CHAIN", p["seq"], off)
            self.records.append(p)
            self.next = p["seq"] + 1; self.count += 1

    def finish(self):
        if self.started and self.first > 1 and self.prune_upto + 1 < self.first: self.fail("PRUNE", self.first)
        if self.want and self.want[0] >= self.first and self.next <= self.want[0]: self.fail("TRUNC", self.next)

def load_pub(s):
    try:
        t = open(s).read().strip()
    except OSError:
        t = s
    b = bytes.fromhex(t)
    if len(b) != 32: raise ValueError("ключ - 32 байта (64 hex-знака)")
    return b

def main(argv):
    pub = None; ck = None; export = None; files = []; show = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--pub": pub = load_pub(argv[i + 1]); i += 2
        elif a == "--ckpt": ck = argv[i + 1]; i += 2
        elif a == "--export": export = argv[i + 1]; i += 2
        elif a == "--list": show = True; i += 1
        elif a in ("-h", "--help"): print(__doc__); return 0
        else: files.append(a); i += 1
    if pub is None or not (files or export): print(__doc__, file=sys.stderr); return 2
    v = V(pub)
    if export:                                    # страницы экспорта: записи без заголовков, проверяются номера, цепочка внутри страницы и подписи точек
        recs = []
        for line in open(export):
            if line.startswith("AUDREC "):
                r = bytes.fromhex(line.split()[1])
                if len(r) != REC: print("плохая строка AUDREC"); return 1
                recs.append(r)
        for k, r in enumerate(recs):
            p = parse(r); v.records.append(p)
            if k and p["seq"] != struct.unpack_from("<Q", recs[k - 1], 0)[0] + 1: v.files = 1; v.fail("SEQ", p["seq"])
            elif k and r[128:160] != rec_hash(recs[k - 1]): v.files = 1; v.fail("CHAIN", p["seq"])
            if p["kind"] == K_CKPT:
                if ckpt_ok(r, pub): v.ckpts += 1
                else: v.files = 1; v.fail("SIG", p["seq"])
        v.count = len(recs); v.first = v.records[0]["seq"] if recs else 0; v.next = v.first + len(recs)
    else:
        if ck:
            w = open(ck, "rb").read()
            if len(w) != REC or not ckpt_ok(w, pub): v.files = 1; v.fail("SIG", 0)
            else: v.want = (struct.unpack_from("<Q", w, 0)[0], rec_hash(w))
        blobs = [(f, open(f, "rb").read()) for f in files]
        # дубликат: текущий файл, начало которого совпадает с последним закрытым (обрыв при ротации), - пропустить
        if len(blobs) >= 2 and blobs[-1][1][16:24] == blobs[-2][1][16:24] and blobs[-1][1][:8] == b"KIRAAUD1":
            a, b = blobs[-1][1], blobs[-2][1]
            if b[:len(a)] == a: blobs.pop()
        for name, b in blobs: v.add_file(name, b)
        v.finish()
    if show:
        for p in v.records:
            print("#%d b%d +%dms %s %s %s pid=%d dom=%d arg=%d %s | %s" % (p["seq"], p["boot"], p["mono"], KINDS[p["kind"]] if p["kind"] < len(KINDS) else "?",
                  SRCS[p["src"]] if p["src"] < len(SRCS) else "?", RES[p["res"]] if p["res"] < len(RES) else "?", p["pid"], p["dom"], p["arg"], p["subj"], p["det"]))
    if v.bad:
        code, seq, f, off = v.bad[0]
        print("НАРУШЕНИЕ: %s: запись %d (файл №%d, смещение %d)" % (WHY[code], seq, f, off))
        if len(v.bad) > 1: print("  ещё нарушений: %d" % (len(v.bad) - 1))
        return 1
    print("OK: записей %d (с %d по %d), подписанных точек проверено %d%s" % (v.count, v.first, v.next - 1, v.ckpts, "" if export else ", файлов %d" % v.files))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
