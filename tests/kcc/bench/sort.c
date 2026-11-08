static int v[200000];
static void qs(int *a,int lo,int hi){ while(lo<hi){ int p=a[(lo+hi)/2],i=lo,j=hi; while(i<=j){ while(a[i]<p)i++; while(a[j]>p)j--; if(i<=j){int t=a[i];a[i]=a[j];a[j]=t;i++;j--;} } if(j-lo<hi-i){qs(a,lo,j);lo=i;}else{qs(a,i,hi);hi=j;} } }
int main(void){ unsigned x=12345; long chk=0; for(int r=0;r<20;r++){ for(int i=0;i<200000;i++){ x=x*1664525u+1013904223u; v[i]=(int)(x>>8); } qs(v,0,199999); for(int i=1;i<200000;i++) if(v[i-1]>v[i]) return 1; chk+=v[100000]; } return (int)(chk&0xffffff); }
