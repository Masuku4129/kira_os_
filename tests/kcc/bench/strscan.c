static char t[1<<20];
static int cnt(const char *s,int n,char c){ int k=0; for(int i=0;i<n;i++) if(s[i]==c) k++; return k; }
static unsigned fnv(const char *s,int n){ unsigned h=2166136261u; for(int i=0;i<n;i++){ h^=(unsigned char)s[i]; h*=16777619u; } return h; }
int main(void){ for(int i=0;i<(1<<20);i++) t[i]=(char)('a'+(i*31+(i>>5))%26); long s=0; for(int r=0;r<80;r++){ s+=cnt(t,1<<20,'e'); s^=fnv(t,1<<20); t[r]=(char)(t[r]+1); } return (int)(s&0xffffff); }
