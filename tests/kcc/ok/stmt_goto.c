int f(int n) { int s = 0; top: if (n <= 0) goto done; s += n; n--; goto top; done: return s; }
