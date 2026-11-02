#!/usr/bin/env python3
"""Генератор модулей WebAssembly для сверки JIT и интерпретатора (tests/wasm_diff.sh).
   ops    - по функции на каждую команду: все пары граничных значений, операнды из констант и из локальных, результат в хэш;
   traps  - по модулю на каждую ловушку (деление, преобразования, память, call_indirect, unreachable);
   random - случайные программы: глубокие выражения (больше регистров, чем есть: вытеснение), ветвления, циклы, вызовы, память.
   Использование: wasm_gen.py ops|traps|random ВЫХОДНАЯ_ПАПКА [семя число]"""
import struct, sys, os, random, math

def uleb(v):
    o = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        if v: o.append(b | 0x80)
        else: o.append(b); return bytes(o)
def sleb(v):
    o = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40): o.append(b); return bytes(o)
        o.append(b | 0x80)
def vec(items): return uleb(len(items)) + b''.join(items)
def section(i, body): return bytes([i]) + uleb(len(body)) + body
def name(s): return uleb(len(s)) + s.encode()
I32, I64, F32, F64 = 0x7F, 0x7E, 0x7D, 0x7C
def ftype(p, r): return b'\x60' + vec([bytes([x]) for x in p]) + vec([bytes([x]) for x in r])

def c32(v): return b'\x41' + sleb(((v & 0xFFFFFFFF) ^ 0x80000000) - 0x80000000)
def c64(v): return b'\x42' + sleb(((v & (2**64 - 1)) ^ 2**63) - 2**63)
def cf32(bits): return b'\x43' + struct.pack('<I', bits)
def cf64(bits): return b'\x44' + struct.pack('<Q', bits)
def lget(i): return b'\x20' + uleb(i)
def lset(i): return b'\x21' + uleb(i)
def ltee(i): return b'\x22' + uleb(i)

def module(funcs, nmem=1, table=None, nglobals=0):
    """funcs: список (типов-индекс, [типы локальных], код); 0-я функция - импорт write; kmain - последняя"""
    types = [ftype([I32, I32], [I32]), ftype([], [I32]), ftype([I32], [I32]), ftype([I32, I32], [I32]), ftype([], [I64])]
    m = b'\0asm\1\0\0\0' + section(1, vec(types))
    m += section(2, vec([name('kira') + name('write') + b'\0' + uleb(0)]))
    m += section(3, vec([uleb(f[0]) for f in funcs]))
    if table: m += section(4, b'\1\x70\0' + uleb(table))
    m += section(5, b'\1\0' + uleb(nmem))
    if nglobals: m += section(6, vec([bytes([I32, 1]) + c32(7 + i) + b'\x0b' for i in range(nglobals)]))
    m += section(7, vec([name('kmain') + b'\0' + uleb(len(funcs))]))   # индекс: импорт 0 + функции 1..n, kmain последняя
    if table: m += section(9, b'\1\0' + c32(0) + b'\x0b' + vec([uleb(1 + i) for i in range(table)]))
    bodies = []
    for (_, loc, code) in funcs:
        ls = vec([uleb(1) + bytes([t]) for t in loc])
        b = ls + code + b'\x0b'
        bodies.append(uleb(len(b)) + b)
    m += section(10, vec(bodies))
    return m

# ---- значения ----
V32 = [0, 1, 2, 7, 31, 32, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xDEADBEEF, 0x12345678, 0xFFFFFFF9]
V64 = [0, 1, 2, 63, 64, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xDEADBEEFCAFEBABE]
def f32b(x): return struct.unpack('<I', struct.pack('<f', x))[0]
def f64b(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
V32F = [0, 0x80000000, f32b(1.0), f32b(-1.0), f32b(0.5), f32b(1.5), f32b(-2.5), f32b(3.4e38), 0x7F800000, 0xFF800000, 0x7FC00000, 1,
        f32b(2147483648.0), f32b(4294967296.0), f32b(-0.9), f32b(123456.789), f32b(8388608.0), f32b(-8388609.0)]
V64F = [0, 0x8000000000000000, f64b(1.0), f64b(-1.0), f64b(0.5), f64b(1.5), f64b(-2.5), f64b(1.7e308), 0x7FF0000000000000, 0xFFF0000000000000,
        0x7FF8000000000000, 1, f64b(2147483648.0), f64b(4294967296.0), f64b(-0.9), f64b(123456.789), f64b(4503599627370496.0), f64b(9.3e18), f64b(1.8e19)]
CONST = {I32: lambda v: c32(v), I64: lambda v: c64(v), F32: cf32, F64: cf64}
VALS = {I32: V32, I64: V64, F32: V32F, F64: V64F}

# ---- таблица команд: (опкод, [типы аргументов], тип результата, префикс FC или None) ----
OPS = {}
def reg(op, args, res, nm=None): OPS[nm or '%02x' % op] = (bytes([op]), args, res)
reg(0x45, [I32], I32); reg(0x67, [I32], I32); reg(0x68, [I32], I32); reg(0x69, [I32], I32); reg(0xC0, [I32], I32); reg(0xC1, [I32], I32)
for o in range(0x46, 0x50): reg(o, [I32, I32], I32)
for o in range(0x6A, 0x79): reg(o, [I32, I32], I32)
reg(0x50, [I64], I32); reg(0x79, [I64], I64); reg(0x7A, [I64], I64); reg(0x7B, [I64], I64); reg(0xC2, [I64], I64); reg(0xC3, [I64], I64); reg(0xC4, [I64], I64)
for o in range(0x51, 0x5B): reg(o, [I64, I64], I32)
for o in range(0x7C, 0x8B): reg(o, [I64, I64], I64)
for o in range(0x5B, 0x61): reg(o, [F32, F32], I32)
for o in range(0x61, 0x67): reg(o, [F64, F64], I32)
for o in range(0x8B, 0x92): reg(o, [F32], F32)
for o in range(0x92, 0x99): reg(o, [F32, F32], F32)
for o in range(0x99, 0xA0): reg(o, [F64], F64)
for o in range(0xA0, 0xA7): reg(o, [F64, F64], F64)
reg(0xA7, [I64], I32); reg(0xAC, [I32], I64); reg(0xAD, [I32], I64)
for o, a, r in [(0xA8, F32, I32), (0xA9, F32, I32), (0xAA, F64, I32), (0xAB, F64, I32), (0xAE, F32, I64), (0xAF, F32, I64), (0xB0, F64, I64), (0xB1, F64, I64)]: reg(o, [a], r)
for o, a, r in [(0xB2, I32, F32), (0xB3, I32, F32), (0xB4, I64, F32), (0xB5, I64, F32), (0xB6, F64, F32), (0xB7, I32, F64), (0xB8, I32, F64), (0xB9, I64, F64),
                (0xBA, I64, F64), (0xBB, F32, F64), (0xBC, F32, I32), (0xBD, F64, I64), (0xBE, I32, F32), (0xBF, I64, F64)]: reg(o, [a], r)
for sub, a, r in [(0, F32, I32), (1, F32, I32), (2, F64, I32), (3, F64, I32), (4, F32, I64), (5, F32, I64), (6, F64, I64), (7, F64, I64)]:
    OPS['fc%d' % sub] = (b'\xfc' + uleb(sub), [a], r)

def fval(t, bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0] if t == F32 else struct.unpack('<d', struct.pack('<Q', bits))[0]

def traps_for(key, args):
    """True, если команда с такими аргументами даёт ловушку (такие тесты - в наборе traps)"""
    ob = int(key, 16) if not key.startswith('fc') else None
    if ob is None: return False
    iv = [a & 0xFFFFFFFF if t == I32 else a for t, a in args]
    if 0x6D <= ob <= 0x70: return iv[1] == 0 or (ob == 0x6D and iv[0] == 0x80000000 and iv[1] == 0xFFFFFFFF)
    if 0x7F <= ob <= 0x82: return iv[1] == 0 or (ob == 0x7F and iv[0] == 1 << 63 and iv[1] == 2**64 - 1)
    if ob in (0xA8, 0xA9, 0xAA, 0xAB, 0xAE, 0xAF, 0xB0, 0xB1):
        t = args[0][0]; x = fval(t, args[0][1])
        if x != x or x in (math.inf, -math.inf): return True
        x = math.trunc(x)
        if ob in (0xA8, 0xAA): return not (-2**31 <= x < 2**31)
        if ob in (0xA9, 0xAB): return not (0 <= x < 2**32)
        if ob in (0xAE, 0xB0): return not (-2**63 <= x < 2**63)
        return not (0 <= x < 2**64)
    return False

HASH = 8   # локальная h:i64; временная t:i64 = 9
def hash_tail(res):
    c = b''
    if res == I32: c += b'\xad'
    elif res == F32: c += b'\xbc\xad'
    elif res == F64: c += b'\xbd'
    return c + lset(9) + lget(HASH) + c64(31) + b'\x7e' + lget(9) + b'\x7c' + lset(HASH)

def gen_ops(outdir):
    names = sorted(OPS)
    # локальные: 0,1:i32  2,3:i64  4,5:f32  6,7:f64  8:h  9:t (i64)
    LOC = {I32: (0, 1), I64: (2, 3), F32: (4, 5), F64: (6, 7)}
    chunk = 5
    for ci in range(0, len(names), chunk):
        funcs = []
        for key in names[ci:ci + chunk]:
            opc, args, res = OPS[key]
            code = c64(0) + lset(HASH)
            combos = [()]
            for t in args: combos = [c + (v,) for c in combos for v in VALS[t]]
            if len(combos) > 150: combos = combos[::2]
            for combo in combos:
                av = list(zip(args, combo))
                if traps_for(key, av): continue
                for pat in range(3):
                    for i, (t, v) in enumerate(av):
                        if pat == 0 or (pat == 1 and i == 0) or (pat == 2 and i == 1):
                            code += CONST[t](v)
                        else:
                            code += CONST[t](v) + lset(LOC[t][i]) + lget(LOC[t][i])
                    code += opc + hash_tail(res)
            code += lget(HASH)
            funcs.append((4, [I32, I32, I64, I64, F32, F32, F64, F64, I64, I64], code))
        n = len(funcs)
        km = b''
        for i in range(n): km += c32(8 * i) + b'\x10' + uleb(1 + i) + b'\x37\x03\x00'
        km += c32(0) + c32(8 * n) + b'\x10\x00' + b'\x1a' + c32(0)
        funcs.append((1, [], km))
        open(os.path.join(outdir, 'ops_%03d.wasm' % (ci // chunk)), 'wb').write(module(funcs))

def gen_traps(outdir):
    cases = []
    def one(nm, code, loc=(), nmem=1, table=None):
        km = code + b'\x1a' + c32(0)
        cases.append((nm, module([(1, list(loc), km)], nmem=nmem, table=table)))
    for ob, nm in [(0x6D, 'i32.div_s'), (0x6E, 'i32.div_u'), (0x6F, 'i32.rem_s'), (0x70, 'i32.rem_u')]:
        for a, b in [(5, 0), (0x80000000, 0xFFFFFFFF), (0x80000000, 1), (5, 0xFFFFFFFF)]:
            one('%s %x %x' % (nm, a, b), c32(a) + c32(b) + bytes([ob]))
            one('%s loc %x %x' % (nm, a, b), c32(a) + lset(0) + c32(b) + lset(1) + lget(0) + lget(1) + bytes([ob]), [I32, I32])
    for ob, nm in [(0x7F, 'i64.div_s'), (0x80, 'i64.div_u'), (0x81, 'i64.rem_s'), (0x82, 'i64.rem_u')]:
        for a, b in [(5, 0), (1 << 63, 2**64 - 1), (1 << 63, 1), (5, 2**64 - 1)]:
            one('%s %x %x' % (nm, a, b), c64(a) + c64(b) + bytes([ob]))
    for key, (opc, args, res) in OPS.items():
        if key.startswith('fc') or int(key, 16) not in (0xA8, 0xA9, 0xAA, 0xAB, 0xAE, 0xAF, 0xB0, 0xB1): continue
        t = args[0]
        for bits in VALS[t]:
            if traps_for(key, [(t, bits)]):
                one('trunc %s %x' % (key, bits), CONST[t](bits) + opc + (b'\xad' if res == I32 else b''), [])
                one('trunc loc %s %x' % (key, bits), CONST[t](bits) + lset(4 if t == F32 else 6) + lget(4 if t == F32 else 6) + opc + (b'\xad' if res == I32 else b''), [I32, I32, I64, I64, F32, F32, F64, F64])
    # память
    for ld, nm in [(b'\x28\x02', 'i32.load'), (b'\x29\x03', 'i64.load'), (b'\x2d\x00', 'load8_u'), (b'\x2f\x01', 'load16_u')]:
        for addr in [65536, 65533, 0xFFFFFFFF, 65535, 0x7FFFFFFC]:
            for off in [0, 1, 4, 0x7FFFFFF0]:
                one('%s addr %x off %x' % (nm, addr, off), c32(addr) + ld[:1] + ld[1:] + uleb(off) + (b'\xad' if ld[0] == 0x28 or ld[0] in (0x2d, 0x2f) else b''))
                one('%s loc addr %x off %x' % (nm, addr, off), c32(addr) + lset(0) + lget(0) + ld[:1] + ld[1:] + uleb(off) + (b'\xad' if ld[0] in (0x28, 0x2d, 0x2f) else b''), [I32])
    for st, vt, nm in [(b'\x36\x02', I32, 'i32.store'), (b'\x37\x03', I64, 'i64.store'), (b'\x3a\x00', I32, 'store8'), (b'\x3b\x01', I32, 'store16')]:
        for addr in [65536, 65533, 0xFFFFFFFF, 65535]:
            one('%s addr %x' % (nm, addr), c32(addr) + CONST[vt](5) + st[:1] + st[1:] + uleb(0) + c64(0) + b'\xa7')
            one('%s loc addr %x' % (nm, addr), c32(addr) + lset(0) + lget(0) + CONST[vt](5) + st[:1] + st[1:] + uleb(0) + c64(0) + b'\xa7', [I32])
    one('unreachable', b'\x00' + c64(0))
    # call_indirect: индекс вне таблицы, пустая запись, несовпадение сигнатуры
    t0 = module([(1, [], c32(5) + b'\x11\x01\x00' + b'\x1a' + c32(0))], table=2)
    cases.append(('call_indirect oob', t0))
    t1 = module([(1, [], c32(1) + b'\x11\x01\x00' + b'\x1a' + c32(0))], table=2)     # запись 1 = функция 2 (kmain), её тип ()->i32 = type 1: подходит -> рекурсия до переполнения
    cases.append(('call_indirect recursion', t1))
    t2 = module([(1, [], c32(9) + c32(0) + b'\x11\x02\x00' + b'\x1a' + c32(0))], table=1)     # type 2 (i32)->i32 против ()->i32
    cases.append(('call_indirect sig', t2))
    # бесконечная рекурсия
    cases.append(('recursion', module([(1, [], b'\x10\x01' + b'\x1a' + c32(0))])))
    for nm, m in cases:
        fn = os.path.join(outdir, 'trap_%03d.wasm' % len(os.listdir(outdir)))
        open(fn, 'wb').write(m)

# ---- случайные программы ----
class Rnd:
    def __init__(self, seed, nf):
        self.r = random.Random(seed); self.nf = nf
    def i32e(self, d, nl, fi):
        """выражение i32 глубины не более d; nl - число локальных i32; fi - индекс функции (вызывать можно только с большим индексом)"""
        r = self.r
        if d <= 0 or r.random() < 0.15:
            k = r.random()
            if k < 0.5 and nl: return lget(r.randrange(nl))
            return c32(r.choice([0, 1, 2, 3, 5, 7, 8, 31, 100, 255, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, r.getrandbits(32)]))
        k = r.randrange(24)
        if k < 9:
            return self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + bytes([r.choice([0x6A, 0x6B, 0x6C, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78])])
        if k < 12:
            return self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + bytes([r.choice(range(0x46, 0x50))])
        if k == 12:   # деление с защитой от нуля и переполнения
            return self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + c32(0x7FFFFFFE) + b'\x71' + c32(1) + b'\x72' + bytes([r.choice([0x6E, 0x70])])
        if k == 13: return self.i32e(d - 1, nl, fi) + bytes([r.choice([0x45, 0x67, 0x68, 0x69, 0xC0, 0xC1])])
        if k == 14: return self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + b'\x1b'
        if k == 15 and nl:   # tee и запись в локальную посреди выражения
            i = r.randrange(nl)
            return self.i32e(d - 1, nl, fi) + ltee(i) + self.i32e(d - 1, nl, fi) + b'\x6a'
        if k == 16:   # загрузка из памяти с маскированным адресом
            return self.i32e(d - 1, nl, fi) + c32(0xFFC) + b'\x71' + bytes([0x28, 2, r.choice([0, 4, 8])])
        if k == 17:   # запись в память, значение выражения - сохранённое
            return (self.i32e(d - 1, nl, fi) + c32(0xFFC) + b'\x71' + self.i32e(d - 1, nl, fi) + bytes([0x36, 2, r.choice([0, 4])]) + self.i32e(d - 1, nl, fi))
        if k == 18:   # if-else с результатом
            return self.i32e(d - 1, nl, fi) + b'\x04\x7f' + self.i32e(d - 1, nl, fi) + b'\x05' + self.i32e(d - 1, nl, fi) + b'\x0b'
        if k == 19:   # блок с br_if и результатом
            return b'\x02\x7f' + self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + b'\x0d\x00' + b'\x1a' + self.i32e(d - 1, nl, fi) + b'\x0b'
        if k == 20 and fi + 1 < self.nf:   # вызов функции с большим индексом (i32,i32)->i32
            callee = r.randrange(fi + 1, self.nf)
            return self.i32e(d - 1, nl, fi) + self.i32e(d - 1, nl, fi) + b'\x10' + uleb(1 + callee)
        if k == 21:   # f32 внутри: (int)(f32 выражение)
            return self.f32e(d - 1, nl, fi) + b'\xbc'
        if k == 22:   # 8/16-битная загрузка
            return self.i32e(d - 1, nl, fi) + c32(0xFFF) + b'\x71' + bytes([r.choice([0x2c, 0x2d, 0x2e, 0x2f]), 0, r.choice([0, 1])])
        return self.i64e(d - 1, nl, fi) + b'\xa7'
    def i64e(self, d, nl, fi):
        r = self.r
        if d <= 0 or r.random() < 0.3: return c64(r.choice([0, 1, 5, 0xFFFFFFFF, 1 << 40, r.getrandbits(64)]))
        k = r.randrange(4)
        if k == 0: return self.i32e(d - 1, nl, fi) + bytes([r.choice([0xAC, 0xAD])])
        if k == 1: return self.i64e(d - 1, nl, fi) + self.i64e(d - 1, nl, fi) + bytes([r.choice([0x7C, 0x7D, 0x7E, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88])])
        if k == 2: return self.i64e(d - 1, nl, fi) + bytes([r.choice([0x79, 0x7A, 0x7B, 0xC2, 0xC3, 0xC4])])
        return self.i64e(d - 1, nl, fi) + self.i64e(d - 1, nl, fi) + c64(r.choice([3, 7, 100])) + b'\x84' + b'\x42\x01\x84' + bytes([r.choice([0x80, 0x82])])
    def f32e(self, d, nl, fi):
        r = self.r
        if d <= 0 or r.random() < 0.2:
            return cf32(f32b(r.choice([0.0, 1.0, -1.0, 0.5, 3.25, -7.5, 100.0, 1e10, 0.1, 255.0])))
        k = r.randrange(7)
        if k < 3: return self.f32e(d - 1, nl, fi) + self.f32e(d - 1, nl, fi) + bytes([r.choice([0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98])])
        if k == 3: return self.f32e(d - 1, nl, fi) + bytes([r.choice([0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91])])
        if k == 4: return self.i32e(d - 1, nl, fi) + bytes([r.choice([0xB2, 0xB3])])
        if k == 5: return self.f32e(d - 1, nl, fi) + self.f32e(d - 1, nl, fi) + bytes([r.choice(range(0x5B, 0x61))]) + b'\xb2'
        return self.f32e(d - 1, nl, fi) + self.f32e(d - 1, nl, fi) + self.f32e(d - 1, nl, fi) + self.f32e(d - 1, nl, fi) + b'\x5d' + b'\x1b'   # select по f32 < ...
    def stmts(self, nl, fi, n, depth=2):
        r = self.r; c = b''
        for _ in range(n):
            k = r.randrange(5)
            if k <= 1: c += self.i32e(3, nl, fi) + lset(r.randrange(nl))
            elif k == 2 and depth > 0:
                c += self.i32e(2, nl, fi) + b'\x04\x40' + self.stmts(nl, fi, 2, depth - 1) + b'\x05' + self.stmts(nl, fi, 2, depth - 1) + b'\x0b'
            elif k == 3 and depth > 0:   # цикл со счётчиком в последней локальной
                cnt = nl - 1
                c += c32(r.randrange(2, 9)) + lset(cnt) + b'\x03\x40' + self.stmts(nl - 1, fi, 2, depth - 1) + lget(cnt) + c32(1) + b'\x6b' + ltee(cnt) + b'\x0d\x00\x0b'
            else: c += self.i32e(2, nl, fi) + b'\x1a'
        return c

def gen_random(outdir, seed, count):
    for n in range(count):
        rn = Rnd(seed * 1000 + n, 6)
        funcs = []
        for fi in range(6):
            nl = 8
            body = b''
            for j in range(2, nl): body += c32(fi * 7 + j) + lset(j)
            body += rn.stmts(nl, fi, 4)
            body += rn.i32e(5, nl, fi)
            for j in range(nl): body += lget(j) + b'\x6a' if j else b''
            funcs.append((3, [I32] * (nl - 2), body))   # (i32,i32)->i32, локальные 2..nl-1
            km = c32(0) + c32(11) + c32(13) + b'\x10\x01' + b'\x36\x02\x00'     # mem[0] = f0(11, 13)
        km += c32(4) + c32(4) + c32(0x55555555) + b'\x10\x02' + b'\x36\x02\x00'
        km += c32(0) + c32(4096) + b'\x10\x00' + b'\x1a'                       # write(mem[0..256))
        km += c32(0)
        funcs.append((1, [], km))
        open(os.path.join(outdir, 'rnd_%04d.wasm' % n), 'wb').write(module(funcs))

def gen_deep(outdir):
    """Глубокие выражения: десятки одновременно живых значений разных типов (больше регистров, чем есть) - проверка вытеснения."""
    conv = {(I32, I64): b'\xac', (I32, F32): b'\xb2', (I32, F64): b'\xb7', (I64, I32): b'\xa7', (I64, F32): b'\xb4', (I64, F64): b'\xb9',
            (F32, I32): b'\xbc', (F32, I64): b'\xbc\xad', (F32, F64): b'\xbb', (F64, I32): b'\xbd\xa7', (F64, I64): b'\xbd', (F64, F32): b'\xb6'}
    addop = {I32: 0x6A, I64: 0x7C, F32: 0x92, F64: 0xA0}; mulop = {I32: 0x6C, I64: 0x7E, F32: 0x94, F64: 0xA2}
    def val(t, i):
        k = i % 8
        if t == I32: return lget(k % 2) + c32(i * 3 + 1) + b'\x6c'
        if t == I64: return lget(k % 2) + b'\xad' + c64(i * 7 + 5) + b'\x7e'
        if t == F32: return lget(k % 2) + b'\xb2' + cf32(f32b(0.5 + i)) + b'\x94'
        return lget(k % 2) + b'\xb7' + cf64(f64b(0.25 + i)) + b'\xa2'
    def gen(n, t, order, i=0):
        if n == 0: return val(t, i)
        t2 = order[(i + 1) % len(order)]
        inner = gen(n - 1, t2, order, i + 1)
        c = inner + (conv.get((t2, t), b'') if t2 != t else b'')
        call = (lget(0) + b'\x10\x01' + (conv.get((I32, t), b'') if t != I32 else b'') + bytes([addop[t]])) if n % 6 == 3 else b''
        return val(t, i) + c + bytes([addop[t] if n % 2 else mulop[t]]) + call
    orders = [[I32, F32, I64, F64], [F64, F32, F64, I32], [I32, I32, F32, F32, I64, I64, F64, F64]]
    for oi, order in enumerate(orders):
        for k in range(3, 33, 2):
            km = c32(7) + lset(0) + c32(0x1234567) + lset(1)
            km += c32(0) + gen(k, order[0], order) + conv.get((order[0], I64), b'') + (b'' if order[0] != I64 else b'') + b'\x37\x03\x00'
            km += c32(0) + c32(8) + b'\x10\x00' + b'\x1a' + c32(0)
            funcs = [(2, [], lget(0) + c32(1) + b'\x6a'), (1, [I32, I32], km)]
            open(os.path.join(outdir, 'deep_%d_%02d.wasm' % (oi, k)), 'wb').write(module(funcs))

if __name__ == '__main__':
    mode, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    if mode == 'ops': gen_ops(out)
    elif mode == 'traps': gen_traps(out)
    elif mode == 'deep': gen_deep(out)
    else: gen_random(out, int(sys.argv[3]) if len(sys.argv) > 3 else 1, int(sys.argv[4]) if len(sys.argv) > 4 else 50)
