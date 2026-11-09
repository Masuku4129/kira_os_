#![no_std]
#[panic_handler] fn panic(_: &core::panic::PanicInfo) -> ! { loop {} }
// --cfg pint: поле b целое (вариант без FP в структуре)
#[cfg(not(pint))] type B = f64;
#[cfg(pint)] type B = i64;
#[repr(C)] #[derive(Clone, Copy)] pub struct P { a: i64, b: B }
extern "C" { fn cpp_h(p: P, n: i64) -> P; }
#[cfg(not(pint))] fn addd(b: B, d: f64) -> B { b + d }
#[cfg(pint)] fn addd(b: B, d: f64) -> B { b + d as i64 }
#[cfg(not(pint))] fn half(b: B) -> B { b + 0.5 }
#[cfg(pint)] fn half(b: B) -> B { b + 0 }
#[no_mangle] pub extern "C" fn rs_g(p: P, i: i32, d: f64) -> P {
    let q = unsafe { cpp_h(P { a: p.a * i as i64 + 1, b: addd(p.b, d) }, 4) };
    P { a: q.a, b: half(q.b) }
}
