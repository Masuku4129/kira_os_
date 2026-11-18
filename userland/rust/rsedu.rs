//! Драйвер Ring 3 на Rust для учебного устройства QEMU «edu» (1234:11e8): то же, что userland/drvedu.c, но на безопасном Rust.
//! Назначение: показать путь для новых драйверов Киры - unsafe только в lib/kira.rs, у драйвера нет ни одного unsafe,
//! обращения к регистрам проходят через проверенные смещения, выход за BAR - паника процесса, а не порча памяти.
//! Проверяет: регистр ID, обратимость LIVE, факториал на устройстве, прерывание MSI и DMA, если права выданы,
//! и отказы ядра на попытки выйти за свои права (чужой BAR, конец BAR, перенос BAR, запретный бит команды).
#![no_std]
#![no_main]
#[path = "lib/kira.rs"] mod kira;
use kira::*;

const ID: u64 = 0x00; const LIVE: u64 = 0x04; const FACT: u64 = 0x08; const STATUS: u64 = 0x20;
const IRQ_RAISED: u64 = 0x24; const IRQ_ACK: u64 = 0x64;
const DMA_SRC: u64 = 0x80; const DMA_DST: u64 = 0x88; const DMA_CNT: u64 = 0x90; const DMA_CMD: u64 = 0x98;
const DEV_BUF: u32 = 0x40000;
const BAR_LEN: u64 = 0x100000;

fn say(s: &str) { write("rsedu: "); write(s); }
fn wait_clear(m: &Mmio, reg: u64, bit: u32) -> bool {
    for _ in 0..1_000_000 { if m.read32(reg) & bit == 0 { return true; } }
    false
}

fn check(name: &str, ok: bool, failed: &mut u32) { say(name); write(if ok { " ok\n" } else { " (FAIL)\n" }); if !ok { *failed += 1; } }

fn run() -> u32 {
    let mut failed = 0;
    let Some(info) = drv_info() else { say("info refused (FAIL)\n"); return 1; };
    say("device "); put_hex(info.vendor as u64); write(":"); put_hex(info.device as u64);
    write(", rights "); put_hex(info.granted as u64); write(if info.flags & KDRV_INFO_MSI != 0 { ", MSI\n" } else { ", no MSI\n" });
    let m = match Mmio::map(0, 0, BAR_LEN) { Ok(m) => m, Err(e) => { say("mmio rc="); put_i64(e); write("\n"); return 1; } };
    let id = m.read32(ID);
    say("id="); put_hex(id as u64); write("\n");
    check("id register", id & 0xFF == 0xED, &mut failed);
    m.write32(LIVE, 0x1234_5678);
    check("liveness", m.read32(LIVE) == !0x1234_5678u32, &mut failed);
    m.write32(FACT, 10);
    let done = wait_clear(&m, STATUS, 1);
    let f = m.read32(FACT);
    say("10!="); put_u64(f as u64); write("\n");
    check("factorial", done && f == 3_628_800, &mut failed);
    if info.flags & KDRV_INFO_MSI != 0 {
        m.write32(STATUS, 0x80);
        m.write32(FACT, 5);
        let ev = drv_irq_wait(300);
        say("irq wait="); put_i64(ev); write(" 5!="); put_u64(m.read32(FACT) as u64); write("\n");
        check("msi", ev == KDRV_EV_IRQ && m.read32(FACT) == 120, &mut failed);
        m.write32(IRQ_ACK, m.read32(IRQ_RAISED));
        m.write32(STATUS, 0);
    } else { say("IRQ/MSI capability not granted (least-privilege PASS)\n"); }
    match Dma::alloc(4096, 30) {
        Ok(mut d) => {
            let phys = d.phys;
            { let b = d.as_mut_slice(); for (i, x) in b.iter_mut().take(256).enumerate() { *x = (i as u8).wrapping_mul(7).wrapping_add(1); } }
            m.write32(DMA_SRC, phys as u32); m.write32(DMA_SRC + 4, 0);
            m.write32(DMA_DST, DEV_BUF); m.write32(DMA_DST + 4, 0);
            m.write32(DMA_CNT, 256); m.write32(DMA_CNT + 4, 0);
            m.write32(DMA_CMD, 1);
            let a = wait_clear(&m, DMA_CMD, 1);
            m.write32(DMA_SRC, DEV_BUF); m.write32(DMA_DST, phys as u32 + 2048); m.write32(DMA_CNT, 256);
            m.write32(DMA_CMD, 3);
            let b = wait_clear(&m, DMA_CMD, 1);
            let s = d.as_slice();
            let same = s[..256] == s[2048..2048 + 256];
            say("dma round trip "); write(if same { "equal\n" } else { "DIFFERENT\n" });
            check("dma", a && b && same, &mut failed);
        }
        Err(e) if e == KE_PERM => say("DMA denied: CAP_DMA not granted (least-privilege PASS)\n"),
        Err(e) => { say("dma rc="); put_i64(e); write("\n"); check("dma", false, &mut failed); }
    }
    // то, что ядро обязано отказать
    let r1 = Mmio::try_map(1, 0, 4096);            // BAR, которого у устройства нет
    let r2 = Mmio::try_map(0, BAR_LEN, 4096);      // за концом своего BAR
    let r3 = drv_cfg_write(0x10, 4, 0xFEE0_0000);  // перенос BAR на чужие адреса
    let r4 = drv_cfg_write(0x04, 2, 0x8);          // бит команды, которого драйверу не дано
    let r5 = drv_cfg_read(0x00, 4);
    say("refused: other-bar="); put_i64(r1); write(" past-bar="); put_i64(r2); write(" bar-move="); put_i64(r3);
    write(" bad-cmd="); put_i64(r4); write(" cfg-id="); put_hex(r5 as u64); write("\n");
    check("kernel guards", r1 < 0 && r2 < 0 && r3 == KE_PERM && r4 == KE_PERM && (r5 & 0xFFFF) == 0x1234, &mut failed);
    failed
}

#[no_mangle]
fn kira_main() -> i32 {
    let failed = run();
    say(if failed == 0 { "PASS\n" } else { "partial (see above)\n" });
    let mut ab = [0u8; 16];
    if args(&mut ab).starts_with("oob") {
        // Намеренный выход за BAR: проверка смещения в Mmio - паника, штатный выход 101, а не обращение мимо устройства.
        say("reading past the BAR on purpose\n");
        let m = Mmio::map(0, 0, 4096).unwrap_or_else(|_| exit(1));
        let _ = m.read32(4096);
    }
    loop { let e = drv_irq_wait(100); if e == KE_INTR { return 0; } if e < 0 { sleep(100); } }
}
