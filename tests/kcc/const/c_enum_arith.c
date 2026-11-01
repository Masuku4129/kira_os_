enum E { A = 3, B, C = A * 4, D = C + B, E_ = -1, F_ };
long long c1 = A;
long long c2 = B;
long long c3 = C;
long long c4 = D;
long long c5 = E_;
long long c6 = F_;
long long c7 = D - A * C;
long long c8 = sizeof(enum E);
