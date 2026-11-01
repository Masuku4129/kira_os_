int f(int x) { switch (x) { case 1: return 10; case 2: case 3: x++; case 4: x += 2; break; default: x = 0; } return x; }
