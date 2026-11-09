// Rust-эквиваленты 4 нагрузок (sha256, sort, matmul int, strscan): no_std, panic=abort, extern "C" точки входа.
// Порт построчный с tests/kcc/bench/*.c, ТЕМ ЖЕ автором и в том же стиле (риск необъективности записан в language-toolchain-matrix.md).
// Индексация: по умолчанию безопасная (с проверкой границ); `--cfg unchecked` заменяет её на get_unchecked (только для замера цены проверок).
#![no_std]
use core::ptr::addr_of_mut;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }

#[cfg(not(unchecked))]
macro_rules! at { ($a:expr, $i:expr) => { $a[$i as usize] }; }
#[cfg(unchecked)]
macro_rules! at { ($a:expr, $i:expr) => { *unsafe { $a.get_unchecked($i as usize) } }; }
#[cfg(not(unchecked))]
macro_rules! atm { ($a:expr, $i:expr) => { $a[$i as usize] }; }
#[cfg(unchecked)]
macro_rules! atm { ($a:expr, $i:expr) => { *unsafe { $a.get_unchecked_mut($i as usize) } }; }

// ---------- sha256 ----------
#[cfg(w_sha256)]
mod m_sha256 {
use super::*;
static K: [u32; 64] = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
fn blk(st: &mut [u32; 8], p: &[u8]) {
    let mut w = [0u32; 64];
    for i in 0..16 { w[i] = (at!(p, 4*i) as u32) << 24 | (at!(p, 4*i+1) as u32) << 16 | (at!(p, 4*i+2) as u32) << 8 | at!(p, 4*i+3) as u32; }
    for i in 16..64 {
        let s0 = w[i-15].rotate_right(7) ^ w[i-15].rotate_right(18) ^ (w[i-15] >> 3);
        let s1 = w[i-2].rotate_right(17) ^ w[i-2].rotate_right(19) ^ (w[i-2] >> 10);
        w[i] = w[i-16].wrapping_add(s0).wrapping_add(w[i-7]).wrapping_add(s1);
    }
    let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h) = (st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
    for i in 0..64 {
        let t1 = h.wrapping_add(e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25)).wrapping_add((e & f) ^ (!e & g)).wrapping_add(K[i]).wrapping_add(w[i]);
        let t2 = (a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22)).wrapping_add((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d.wrapping_add(t1); d = c; c = b; b = a; a = t1.wrapping_add(t2);
    }
    st[0] = st[0].wrapping_add(a); st[1] = st[1].wrapping_add(b); st[2] = st[2].wrapping_add(c); st[3] = st[3].wrapping_add(d);
    st[4] = st[4].wrapping_add(e); st[5] = st[5].wrapping_add(f); st[6] = st[6].wrapping_add(g); st[7] = st[7].wrapping_add(h);
}
static mut BUF: [u8; 64 * 1024] = [0; 64 * 1024];
#[no_mangle]
pub extern "C" fn rs_sha256() -> i32 {
    let mut st: [u32; 8] = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
    let buf = unsafe { &mut *addr_of_mut!(BUF) };
    for i in 0..buf.len() { buf[i] = (i as u32).wrapping_mul(7).wrapping_add(3) as u8; }
    for _ in 0..60 { let mut o = 0; while o < buf.len() { blk(&mut st, &buf[o..o + 64]); o += 64; } }
    ((st[0] ^ st[3] ^ st[7]) & 0xffffff) as i32
}
}

// ---------- sort ----------
#[cfg(w_sort)]
mod m_sort {
use super::*;
static mut V: [i32; 200000] = [0; 200000];
fn qs(a: &mut [i32], mut lo: i32, mut hi: i32) {
    while lo < hi {
        let p = at!(a, (lo + hi) / 2);
        let (mut i, mut j) = (lo, hi);
        while i <= j {
            while at!(a, i) < p { i += 1; }
            while at!(a, j) > p { j -= 1; }
            if i <= j { let t = at!(a, i); atm!(a, i) = at!(a, j); atm!(a, j) = t; i += 1; j -= 1; }
        }
        if j - lo < hi - i { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
    }
}
#[no_mangle]
pub extern "C" fn rs_sort() -> i32 {
    let v = unsafe { &mut *addr_of_mut!(V) };
    let mut x: u32 = 12345; let mut chk: i64 = 0;
    for _ in 0..20 {
        for i in 0..200000 { x = x.wrapping_mul(1664525).wrapping_add(1013904223); v[i] = (x >> 8) as i32; }
        qs(v, 0, 199999);
        for i in 1..200000 { if v[i-1] > v[i] { return 1; } }
        chk += v[100000] as i64;
    }
    (chk & 0xffffff) as i32
}
}

// ---------- matmul (int 120x120) ----------
#[cfg(w_matmul)]
mod m_matmul {
use super::*;
static mut MA: [[i32; 120]; 120] = [[0; 120]; 120];
static mut MB: [[i32; 120]; 120] = [[0; 120]; 120];
static mut MC: [[i32; 120]; 120] = [[0; 120]; 120];
#[no_mangle]
pub extern "C" fn rs_matmul() -> i32 {
    let (a, b, c) = unsafe { (&mut *addr_of_mut!(MA), &mut *addr_of_mut!(MB), &mut *addr_of_mut!(MC)) };
    for i in 0..120 { for j in 0..120 { a[i][j] = (i + j) as i32; b[i][j] = i as i32 - j as i32; } }
    let mut t: i64 = 0;
    for r in 0..40 {
        for i in 0..120 { for j in 0..120 { let mut s: i32 = 0; for k in 0..120 { s = s.wrapping_add(a[i][k].wrapping_mul(b[k][j])); } c[i][j] = s; } }
        t += c[r][r] as i64;
    }
    (t & 0xffffff) as i32
}
}

// ---------- strscan ----------
#[cfg(w_strscan)]
mod m_strscan {
use super::*;
static mut T: [u8; 1 << 20] = [0; 1 << 20];
fn cnt(s: &[u8], c: u8) -> i32 { let mut k = 0; for i in 0..s.len() { if at!(s, i) == c { k += 1; } } k }
fn fnv(s: &[u8]) -> u32 { let mut h: u32 = 2166136261; for i in 0..s.len() { h ^= at!(s, i) as u32; h = h.wrapping_mul(16777619); } h }
#[no_mangle]
pub extern "C" fn rs_strscan() -> i32 {
    let t = unsafe { &mut *addr_of_mut!(T) };
    for i in 0..(1usize << 20) { t[i] = b'a' + ((i * 31 + (i >> 5)) % 26) as u8; }
    let mut s: i64 = 0;
    for r in 0..80 { s += cnt(t, b'e') as i64; s ^= fnv(t) as i64; t[r] = t[r].wrapping_add(1); }
    (s & 0xffffff) as i32
}
}

