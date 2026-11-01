int (*sel(int x))(int);
int (*sel(int x))(int) { return (int (*)(int))8; }
