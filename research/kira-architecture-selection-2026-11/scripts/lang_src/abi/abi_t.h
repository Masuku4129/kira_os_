/* набор тестов SysV AMD64: f_* определены в одном компиляторе (abi_impl.c), вызываются из другого (abi_call.c). Без #include: собирается kcc, clang, gcc. */
typedef __builtin_va_list va_list;
typedef struct { int a, b; } S8;                   /* 8 байт: один INTEGER-регистр */
typedef struct { long a, b; } S16;                 /* два INTEGER-регистра */
typedef struct { long a, b, c, d, e; } S40;        /* >16 байт: память (sret / стек) */
typedef struct { double x, y; } D16;               /* два SSE-регистра */
typedef struct { long a; double b; } M16;          /* INTEGER + SSE */
typedef struct { float x, y, z; } F12;             /* 12 байт из float: xmm0 (x,y) + xmm1 (z) */
typedef struct { char c; short s; int i; } Cm;     /* упакованные малые поля: один регистр */
S8   f_s8(S8 s, int k);
S16  f_s16(S16 s, long k);
S40  f_s40(S40 s, long k);
D16  f_d16(D16 d, double k);
M16  f_m16(M16 m, int k);
F12  f_f12(F12 f, float k);
Cm   f_cm(Cm c, int k);
double f_many_d(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j);
long   f_many_i(long a, long b, long c, long d, long e, long f, long g, long h, long i);
long   f_mix(int a, double b, long c, float d, char e, short f, double g, unsigned char h);
long   f_ext(signed char a, unsigned char b, short c, unsigned short d, int e);
signed char f_ret_sc(int x);
unsigned short f_ret_us(int x);
long   f_vsum(int n, ...);                         /* вариативная: int и double вперемешку по таблице типов в первом аргументе-маске */
double f_vd(int n, ...);
long   f_cb(long (*cb)(long, long), long x);       /* указатель на функцию другой стороны */
long   f_cbtarget(long a, long b);
