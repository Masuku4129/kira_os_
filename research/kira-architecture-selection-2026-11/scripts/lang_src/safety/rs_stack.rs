use std::hint::black_box;
fn main() {
    let n = black_box(8usize); let guard = black_box(1111);
    let mut a = [0i32; 8];
    let unchecked = std::env::args().count() > 1;      /* без аргумента - безопасная индексация; с аргументом - unsafe get_unchecked_mut */
    for i in 0..=n + 3 { if unchecked { unsafe { *a.get_unchecked_mut(i) = 7777; } } else { a[i] = 7777; } }
    println!("guard={} a0={}", guard, a[0]);
}
