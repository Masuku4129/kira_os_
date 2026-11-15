//! Rust (no_std) на Kira OS: программа Ring 3 без стандартной библиотеки и без libc.
//! Показывает путь для новых безопасных служб и драйверов: тонкая обёртка над системными вызовами (unsafe только в ней),
//! остальной код - безопасный Rust. Самопроверка: SHA-256 («abc» и длинное сообщение), сортировка, кольцевой буфер,
//! проверка границ без паники ядра (паника = штатное завершение процесса с кодом 101).
#![no_std]
#![no_main]

use core::arch::{asm, global_asm};
use core::panic::PanicInfo;

// --- уровень 7: системные вызовы Kira (номера как в userland/kira.h) ---
mod sys {
    use super::asm;
    const K_WRITE: u64 = 1;
    const K_EXIT: u64 = 2;
    const K_GETPID: u64 = 6;

    #[inline(always)]
    unsafe fn call(n: u64, a: u64, b: u64, c: u64) -> i64 {
        let r: i64;
        asm!("syscall", inlateout("rax") n as i64 => r, in("rdi") a, in("rsi") b, in("rdx") c,
             lateout("rcx") _, lateout("r11") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
             options(nostack));
        r
    }
    pub fn write(s: &str) { unsafe { call(K_WRITE, s.as_ptr() as u64, s.len() as u64, 0); } }
    pub fn getpid() -> u64 { unsafe { call(K_GETPID, 0, 0, 0) as u64 } }
    pub fn exit(code: i32) -> ! { loop { unsafe { call(K_EXIT, code as u64, 0, 0); } } }
}

// Вывод чисел без форматировщика core::fmt (меньше кода и нет скрытых зависимостей)
fn put_u64(mut v: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    loop { i -= 1; buf[i] = b'0' + (v % 10) as u8; v /= 10; if v == 0 { break; } }
    if let Ok(s) = core::str::from_utf8(&buf[i..]) { sys::write(s); }
}
fn put_hex(bytes: &[u8]) {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut line = [0u8; 64];
    for (i, b) in bytes.iter().take(32).enumerate() { line[2 * i] = H[(b >> 4) as usize]; line[2 * i + 1] = H[(b & 15) as usize]; }
    if let Ok(s) = core::str::from_utf8(&line[..2 * bytes.len().min(32)]) { sys::write(s); }
}

// --- SHA-256 (FIPS 180-4) ---
const K: [u32; 64] = [
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
];

struct Sha256 { h: [u32; 8], buf: [u8; 64], n: usize, total: u64 }
impl Sha256 {
    fn new() -> Self {
        Sha256 { h: [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19], buf: [0; 64], n: 0, total: 0 }
    }
    fn block(&mut self, p: &[u8; 64]) {
        let mut w = [0u32; 64];
        for i in 0..16 { w[i] = u32::from_be_bytes([p[4*i], p[4*i+1], p[4*i+2], p[4*i+3]]); }
        for i in 16..64 {
            let s0 = w[i-15].rotate_right(7) ^ w[i-15].rotate_right(18) ^ (w[i-15] >> 3);
            let s1 = w[i-2].rotate_right(17) ^ w[i-2].rotate_right(19) ^ (w[i-2] >> 10);
            w[i] = w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
        }
        let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = self.h;
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = h.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let mj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(mj);
            h = g; g = f; f = e; e = d.wrapping_add(t1); d = c; c = b; b = a; a = t1.wrapping_add(t2);
        }
        for (x, y) in self.h.iter_mut().zip([a, b, c, d, e, f, g, h]) { *x = x.wrapping_add(y); }
    }
    fn update(&mut self, mut d: &[u8]) {
        self.total += d.len() as u64;
        while !d.is_empty() {
            let take = (64 - self.n).min(d.len());
            self.buf[self.n..self.n + take].copy_from_slice(&d[..take]);
            self.n += take; d = &d[take..];
            if self.n == 64 { let b = self.buf; self.block(&b); self.n = 0; }
        }
    }
    fn finish(mut self) -> [u8; 32] {
        let bits = self.total * 8;
        self.update(&[0x80]);
        while self.n != 56 { self.update(&[0]); }
        self.update(&bits.to_be_bytes());
        let mut out = [0u8; 32];
        for (i, v) in self.h.iter().enumerate() { out[4*i..4*i+4].copy_from_slice(&v.to_be_bytes()); }
        out
    }
}

// --- безопасная структура: кольцевой буфер фиксированного размера, без выделения памяти ---
struct Ring<const N: usize> { d: [u32; N], head: usize, len: usize }
impl<const N: usize> Ring<N> {
    const fn new() -> Self { Ring { d: [0; N], head: 0, len: 0 } }
    fn push(&mut self, v: u32) -> Result<(), u32> {
        if self.len == N { return Err(v); }
        self.d[(self.head + self.len) % N] = v; self.len += 1; Ok(())
    }
    fn pop(&mut self) -> Option<u32> {
        if self.len == 0 { return None; }
        let v = self.d[self.head]; self.head = (self.head + 1) % N; self.len -= 1; Some(v)
    }
}

fn check(name: &str, ok: bool, failed: &mut u32) {
    sys::write("rsdemo: "); sys::write(name); sys::write(if ok { " ok\n" } else { " FAIL\n" });
    if !ok { *failed += 1; }
}

fn run() -> u32 {
    let mut failed = 0;
    sys::write("rsdemo: Rust no_std on Kira OS, pid "); put_u64(sys::getpid()); sys::write("\n");

    let mut h = Sha256::new(); h.update(b"abc");
    let d = h.finish();
    const ABC: [u8; 32] = [0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad];
    check("sha256(abc)", d == ABC, &mut failed);
    sys::write("rsdemo: digest "); put_hex(&d); sys::write("\n");

    // 1 000 000 байт «a»: известный контрольный дайджест cdc76e5c...
    let mut h = Sha256::new(); let chunk = [b'a'; 1000];
    for _ in 0..1000 { h.update(&chunk); }
    let d = h.finish();
    const MILLION: [u8; 8] = [0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92];
    check("sha256(1e6 x 'a')", d[..8] == MILLION, &mut failed);

    let mut v = [9u32, 3, 7, 1, 8, 2, 6, 4, 5, 0];
    v.sort_unstable();
    check("sort", v.iter().enumerate().all(|(i, &x)| x as usize == i), &mut failed);

    let mut r: Ring<4> = Ring::new();
    let mut ok = true;
    for i in 0..4 { ok &= r.push(i).is_ok(); }
    ok &= r.push(99) == Err(99);
    ok &= r.pop() == Some(0);
    ok &= r.push(4).is_ok();
    let mut got = [0u32; 4];
    for g in got.iter_mut() { *g = r.pop().unwrap_or(u32::MAX); }
    ok &= got == [1, 2, 3, 4] && r.pop().is_none();
    check("ring buffer", ok, &mut failed);

    // проверка границ: индекс из «внешних данных» - get() возвращает None, без паники и без выхода за массив
    let idx = (sys::getpid() as usize).wrapping_add(1000);
    check("bounds", v.get(idx).is_none(), &mut failed);
    failed
}

#[no_mangle]
pub extern "C" fn rmain() -> ! {
    let failed = run();
    if failed == 0 { sys::write("rsdemo: all checks passed (PASS)\n"); sys::exit(0) }
    sys::write("rsdemo: FAILED\n");
    sys::exit(1)
}

global_asm!(".section .text.start", ".global _start", "_start:", "and rsp, -16", "call rmain", "ud2");

#[panic_handler]
fn panic(_: &PanicInfo) -> ! { sys::write("rsdemo: panic\n"); sys::exit(101) }

// core требует эти функции от среды; без libc они наши (побайтово, чтобы компилятор не подменил цикл вызовом самой себя)
#[no_mangle] pub unsafe extern "C" fn memcpy(d: *mut u8, s: *const u8, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), *s.add(i)); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memmove(d: *mut u8, s: *const u8, n: usize) -> *mut u8 {
    if (d as usize) <= (s as usize) { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), *s.add(i)); i += 1; } }
    else { let mut i = n; while i > 0 { i -= 1; core::ptr::write_volatile(d.add(i), *s.add(i)); } }
    d
}
#[no_mangle] pub unsafe extern "C" fn memset(d: *mut u8, c: i32, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), c as u8); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    let mut i = 0; while i < n { let (x, y) = (*a.add(i), *b.add(i)); if x != y { return x as i32 - y as i32; } i += 1; } 0
}
#[no_mangle] pub unsafe extern "C" fn bcmp(a: *const u8, b: *const u8, n: usize) -> i32 { memcmp(a, b, n) }

// Предкомпилированный core ссылается на личность раскрутки стека; при panic=abort она не вызывается никогда.
#[no_mangle] pub extern "C" fn rust_eh_personality() { sys::exit(102) }
