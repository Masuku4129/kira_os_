enum E { A, B = 5, C };
int f(enum E e) { return e == C ? A : B; }
