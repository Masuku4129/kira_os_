// unsafe Rust: то же использование после освобождения через сырой указатель
fn main() {
    let b = Box::new(1234i32); let p = Box::into_raw(b);
    unsafe { drop(Box::from_raw(p)); }
    let q = Box::new(5678i32);
    println!("*p={} q={} same={}", unsafe { std::ptr::read_volatile(p) }, q, std::ptr::eq(p, &*q));
}
