static signed char W[256][256]; static int x[256], y[256];
int main(void){ unsigned r=7; for(int i=0;i<256;i++){ for(int j=0;j<256;j++){ r=r*1103515245u+12345u; W[i][j]=(signed char)(r>>16); } x[i]=i-128; }
 long s=0; for(int it=0;it<3000;it++){ for(int i=0;i<256;i++){ int a=0; for(int j=0;j<256;j++) a+=W[i][j]*x[j]; y[i]=a>>8; } for(int i=0;i<256;i++){ x[i]=y[i]>127?127:(y[i]<-128?-128:y[i]); s+=x[i]; } } return (int)(s&0xffffff); }
