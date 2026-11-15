//! Kira OS для Rust (no_std): системные вызовы Ring 3 (номера - userland/kira.h и kdrv_sysnr.h, договор с ядром).
//! unsafe живёт только здесь; программы и драйверы пишутся на безопасном Rust. Подключение: `#[path = "lib/kira.rs"] mod kira;`.
//! Здесь же точка входа, обработчик паники (штатный выход с кодом 101) и mem*-функции, которые нужны core без libc.
#![allow(dead_code)]
use core::arch::{asm, global_asm};
use core::panic::PanicInfo;

pub const K_WRITE: u64 = 1;
pub const K_EXIT: u64 = 2;
pub const K_TICKS: u64 = 3;
pub const K_SLEEP: u64 = 4;
pub const K_YIELD: u64 = 5;
pub const K_GETPID: u64 = 6;
pub const K_ARGS: u64 = 30;
pub const KDRV_SYS_INFO: u64 = 48;
pub const KDRV_SYS_CFG_READ: u64 = 49;
pub const KDRV_SYS_CFG_WRITE: u64 = 50;
pub const KDRV_SYS_MMIO_MAP: u64 = 51;
pub const KDRV_SYS_DMA_ALLOC: u64 = 52;
pub const KDRV_SYS_IRQ_WAIT: u64 = 53;

pub const KE_PERM: i64 = -12;
pub const KE_TIMEOUT: i64 = -5;
pub const KE_INTR: i64 = -11;
pub const KDRV_EV_IRQ: i64 = 1;
pub const KDRV_INFO_MSI: u32 = 1;
pub const K_FOREVER: u64 = 0xFFFF_FFFF;

#[inline(always)]
pub fn syscall(n: u64, a: u64, b: u64, c: u64) -> i64 {
    let r: i64;
    // Договор x86-64: номер в rax, аргументы rdi/rsi/rdx; ядро портит rcx, r11 и r8-r10.
    unsafe {
        asm!("syscall", inlateout("rax") n as i64 => r, in("rdi") a, in("rsi") b, in("rdx") c,
             lateout("rcx") _, lateout("r11") _, lateout("r8") _, lateout("r9") _, lateout("r10") _,
             options(nostack));
    }
    r
}

pub fn write(s: &str) { syscall(K_WRITE, s.as_ptr() as u64, s.len() as u64, 0); }
pub fn getpid() -> u64 { syscall(K_GETPID, 0, 0, 0) as u64 }
pub fn ticks() -> u64 { syscall(K_TICKS, 0, 0, 0) as u64 }
pub fn sleep(ticks: u64) { syscall(K_SLEEP, ticks, 0, 0); }
pub fn exit(code: i32) -> ! { loop { syscall(K_EXIT, code as u64, 0, 0); } }
/// Аргументы запуска в буфер вызывающего; возвращает их как строку (пустую при ошибке).
pub fn args(buf: &mut [u8]) -> &str {
    let n = syscall(K_ARGS, buf.as_mut_ptr() as u64, buf.len() as u64, 0);
    let n = if n < 0 { 0 } else { (n as usize).min(buf.len()) };
    let n = buf[..n].iter().position(|&b| b == 0).unwrap_or(n);
    core::str::from_utf8(&buf[..n]).unwrap_or("")
}

// --- вывод без core::fmt (меньше кода, нет скрытых зависимостей) ---
pub fn put_u64(mut v: u64) {
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    loop { i -= 1; buf[i] = b'0' + (v % 10) as u8; v /= 10; if v == 0 { break; } }
    if let Ok(s) = core::str::from_utf8(&buf[i..]) { write(s); }
}
pub fn put_i64(v: i64) { if v < 0 { write("-"); put_u64(v.unsigned_abs()); } else { put_u64(v as u64); } }
pub fn put_hex(v: u64) {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut buf = [0u8; 18]; buf[0] = b'0'; buf[1] = b'x';
    let mut n = 2; let mut started = false;
    for s in (0..16).rev() {
        let d = ((v >> (4 * s)) & 15) as usize;
        if d != 0 || started || s == 0 { buf[n] = H[d]; n += 1; started = true; }
    }
    if let Ok(s) = core::str::from_utf8(&buf[..n]) { write(s); }
}
pub fn put_hex_bytes(bytes: &[u8]) {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut line = [0u8; 64];
    let n = bytes.len().min(32);
    for (i, b) in bytes.iter().take(n).enumerate() { line[2 * i] = H[(b >> 4) as usize]; line[2 * i + 1] = H[(b & 15) as usize]; }
    if let Ok(s) = core::str::from_utf8(&line[..2 * n]) { write(s); }
}

// --- драйверы Ring 3 (kdrv_format.h): каждый вызов действует только на устройство, закреплённое ядром за этим процессом ---
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DrvBar { pub size: u64, pub flags: u32, pub pad: u32 }
/// Раскладка строго как kdrv_info_t в kdrv_format.h (ядро проверяет размер).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DrvInfo { pub size: u32, pub vendor: u16, pub device: u16, pub cls: u32, pub granted: u32, pub flags: u32, pub version: u32,
                     pub name: [u8; 16], pub bar: [DrvBar; 6] }
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DmaBlock { pub va: u64, pub phys: u64, pub bytes: u64 }

pub fn drv_info() -> Option<DrvInfo> {
    let mut i = DrvInfo::default();
    i.size = core::mem::size_of::<DrvInfo>() as u32;
    if syscall(KDRV_SYS_INFO, &mut i as *mut DrvInfo as u64, i.size as u64, 0) == 0 { Some(i) } else { None }
}
pub fn drv_cfg_read(off: u32, width: u32) -> i64 { syscall(KDRV_SYS_CFG_READ, off as u64, width as u64, 0) }
pub fn drv_cfg_write(off: u32, width: u32, v: u32) -> i64 { syscall(KDRV_SYS_CFG_WRITE, off as u64, width as u64, v as u64) }
pub fn drv_irq_wait(timeout10ms: u64) -> i64 { syscall(KDRV_SYS_IRQ_WAIT, timeout10ms, 0, 0) }

/// Отображение BAR: регистры читаются и пишутся только volatile-доступом по проверенному смещению.
pub struct Mmio { base: *mut u8, len: u64 }
impl Mmio {
    pub fn map(bar: u32, off: u64, len: u64) -> Result<Mmio, i64> {
        let r = syscall(KDRV_SYS_MMIO_MAP, bar as u64, off, len);
        if r < 0 { Err(r) } else { Ok(Mmio { base: r as u64 as *mut u8, len }) }
    }
    /// Сырой запрос (для проверки отказов ядра): код результата без создания отображения.
    pub fn try_map(bar: u32, off: u64, len: u64) -> i64 { syscall(KDRV_SYS_MMIO_MAP, bar as u64, off, len) }
    pub fn read32(&self, off: u64) -> u32 {
        assert!(off + 4 <= self.len && off % 4 == 0);
        unsafe { core::ptr::read_volatile(self.base.add(off as usize) as *const u32) }
    }
    pub fn write32(&self, off: u64, v: u32) {
        assert!(off + 4 <= self.len && off % 4 == 0);
        unsafe { core::ptr::write_volatile(self.base.add(off as usize) as *mut u32, v) }
    }
}

/// Память для DMA: обнулённая, непрерывная, освобождается ядром при завершении драйвера.
pub struct Dma { pub phys: u64, buf: *mut u8, len: usize }
impl Dma {
    pub fn alloc(bytes: u64, addr_bits: u32) -> Result<Dma, i64> {
        let mut d = DmaBlock::default();
        let r = syscall(KDRV_SYS_DMA_ALLOC, bytes, addr_bits as u64, &mut d as *mut DmaBlock as u64);
        if r < 0 { Err(r) } else { Ok(Dma { phys: d.phys, buf: d.va as *mut u8, len: d.bytes as usize }) }
    }
    pub fn as_slice(&self) -> &[u8] { unsafe { core::slice::from_raw_parts(self.buf, self.len) } }
    pub fn as_mut_slice(&mut self) -> &mut [u8] { unsafe { core::slice::from_raw_parts_mut(self.buf, self.len) } }
}

// --- среда выполнения: вход, паника, память ---
extern "Rust" { fn kira_main() -> i32; }
#[no_mangle]
pub extern "C" fn kira_start() -> ! { let code = unsafe { kira_main() }; exit(code) }
global_asm!(".section .text.start", ".global _start", "_start:", "and rsp, -16", "call kira_start", "ud2");

#[panic_handler]
fn panic(_: &PanicInfo) -> ! { write("rust: panic\n"); exit(101) }

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
#[no_mangle] pub extern "C" fn rust_eh_personality() { exit(102) }
