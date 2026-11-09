use std::hint::black_box;
fn main() {
    let n = black_box(8usize);
    let mut a = vec![1i32; 8]; let b = vec![100i32; 8];
    let unchecked = std::env::args().count() > 1;
    for i in 0..=n + 3 { if unchecked { unsafe { *a.as_mut_ptr().add(i) = 7777; } } else { a[i] = 7777; } }
    println!("b0={} b1={}", b[0], b[1]);
}
