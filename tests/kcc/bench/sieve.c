static unsigned char s[4000000];
int main(void){ long c=0; for(int r=0;r<12;r++){ for(long i=0;i<4000000;i++) s[i]=1; for(long i=2;i<4000000;i++) if(s[i]){ c++; for(long j=i*2;j<4000000;j+=i) s[j]=0; } } return (int)(c&0xffffff); }
