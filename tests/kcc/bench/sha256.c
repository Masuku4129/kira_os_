static const unsigned K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define R(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void blk(unsigned *st,const unsigned char *p){ unsigned w[64],a,b,c,d,e,f,g,h,t1,t2; for(int i=0;i<16;i++) w[i]=(unsigned)p[4*i]<<24|(unsigned)p[4*i+1]<<16|(unsigned)p[4*i+2]<<8|p[4*i+3];
 for(int i=16;i<64;i++){ unsigned s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3), s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
 a=st[0];b=st[1];c=st[2];d=st[3];e=st[4];f=st[5];g=st[6];h=st[7];
 for(int i=0;i<64;i++){ t1=h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+K[i]+w[i]; t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
 st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h; }
static unsigned char buf[64*1024];
int main(void){ unsigned st[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; for(int i=0;i<(int)sizeof buf;i++) buf[i]=(unsigned char)(i*7+3);
 for(int r=0;r<60;r++) for(int o=0;o<(int)sizeof buf;o+=64) blk(st,buf+o); return (int)((st[0]^st[3]^st[7])&0xffffff); }
