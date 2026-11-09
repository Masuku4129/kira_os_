use std::hint::black_box;
fn main() {
    let x: i32 = black_box(i32::MAX);
    let y = x + 1;                         /* debug: паника overflow; release: оборачивание (два дополнения) */
    println!("y={} (x+1>x)={}", y, y > x);
}
