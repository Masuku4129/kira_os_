// безопасный Rust: изменение общего счётчика из двух потоков без синхронизации НЕ компилируется
use std::thread;
fn main() {
    let mut counter = 0u64;
    let a = thread::spawn(|| { for _ in 0..2000000 { counter += 1; } });
    let b = thread::spawn(|| { for _ in 0..2000000 { counter += 1; } });
    a.join().unwrap(); b.join().unwrap();
    println!("counter={}", counter);
}
