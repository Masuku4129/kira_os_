static int A[120][120],B[120][120],C[120][120];
int main(void){ for(int i=0;i<120;i++)for(int j=0;j<120;j++){A[i][j]=i+j;B[i][j]=i-j;}
 long t=0; for(int r=0;r<40;r++){ for(int i=0;i<120;i++)for(int j=0;j<120;j++){int s=0;for(int k=0;k<120;k++)s+=A[i][k]*B[k][j];C[i][j]=s;} t+=C[r][r]; } return (int)(t&0xffffff); }
