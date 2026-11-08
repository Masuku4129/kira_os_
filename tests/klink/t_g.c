/* компилируется без -fvisibility=hidden: доступ к внешнему символу через GOT (R_X86_64_REX_GOTPCRELX), которую klink релаксирует в lea */
extern int data_val;
extern int bump(void);
int got_probe(void) { return data_val; }
int got_lazy_ok(void) { int *p = &data_val; return *p == 41 && p != 0; }
