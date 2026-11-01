enum { X = 1 << 3, Y = X | 2, Z = (Y > 5) ? Y : 0, W = sizeof(int) * 4 };
long long c1 = X;
long long c2 = Y;
long long c3 = Z;
long long c4 = W;
