enum A { A0 };
enum B { B0 };
long f(enum A *a, enum B *b) { return a - b; }
