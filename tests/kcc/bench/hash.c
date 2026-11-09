static unsigned rotr(unsigned x,int n){return (x>>n)|(x<<(32-n));}
int main(void){ unsigned h=0x6a09e667,a=1,b=2; for(long i=0;i<60000000;i++){ a=rotr(a^h,7)+b; b=rotr(b+a,13)^i; h+=a*31u+(b>>3); } return (int)(h&0xffffff); }
