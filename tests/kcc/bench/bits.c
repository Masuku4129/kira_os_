static int pop(unsigned long x){ int c=0; while(x){ x&=x-1; c++; } return c; }
static int ctz(unsigned long x){ int c=0; if(!x) return 64; while(!(x&1)){ x>>=1; c++; } return c; }
int main(void){ unsigned long x=88172645463325252UL; long s=0; for(long i=0;i<12000000;i++){ x^=x<<13; x^=x>>7; x^=x<<17; s+=pop(x)+ctz(x|1UL<<40); } return (int)(s&0xffffff); }
