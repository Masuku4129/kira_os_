// unsafe Rust: гонка через static mut (UB в Rust тоже)
use std::thread;
static mut COUNTER: u64 = 0;
fn main() {
    let w = || { for _ in 0..2000000 { unsafe { let c = std::ptr::read_volatile(&raw const COUNTER); std::ptr::write_volatile(&raw mut COUNTER, c + 1); } } };
    let a = thread::spawn(w); let b = thread::spawn(w);
    a.join().unwrap(); b.join().unwrap();
    println!("counter={} (ожидалось 4000000)", unsafe { std::ptr::read_volatile(&raw const COUNTER) });
}
