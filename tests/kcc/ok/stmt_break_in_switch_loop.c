int f(int n) { for (int i = 0; i < n; i++) { switch (i) { case 2: continue; case 5: break; } if (i == 7) break; } return n; }
