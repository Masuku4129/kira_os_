static int fib(int n){ return n<2?n:fib(n-1)+fib(n-2); }
static int sq(int x){ return x*x+1; }
int main(void){ long s=fib(36); for(int i=0;i<100000000;i++) s+=sq(i&255); return (int)(s&0xffffff); }
