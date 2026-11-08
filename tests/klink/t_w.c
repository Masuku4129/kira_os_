/* слабая неопределённая ссылка: адрес должен быть 0 (-fno-pic: R_X86_64_32S против слабого неопределённого символа) */
extern int never_defined(void) __attribute__((weak));
int weak_probe(void) { return never_defined == 0 ? 77 : -1; }
