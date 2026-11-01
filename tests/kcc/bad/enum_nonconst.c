int g(void);
enum E { A = 1, B = A + 1 };
int h(int n) { enum { C = n }; return C; }
