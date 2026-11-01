int f(void) { int s = 0; for (int i = 0; i < 4; i++) for (int j = 0; j < i; j++) s += i * j; return s; }
