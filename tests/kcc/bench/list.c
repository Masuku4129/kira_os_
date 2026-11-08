typedef struct N { struct N *next; int key; int pad; } N;
static N pool[100000];
static N *rev(N *h){ N *p=0; while(h){ N *n=h->next; h->next=p; p=h; h=n; } return p; }
int main(void){ long sum=0; for(int r=0;r<400;r++){ N *h=0; for(int i=0;i<100000;i++){ pool[i].key=i^r; pool[i].next=h; h=&pool[i]; } h=rev(h); for(N *p=h;p;p=p->next) sum+=p->key&15; h=rev(h); for(N *p=h;p;p=p->next) sum^=p->key; } return (int)(sum&0xffffff); }
