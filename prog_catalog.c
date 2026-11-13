#include "prog_catalog.h"
#include "app_catalog.h"
static uint32_t decode(const unsigned char** p) {
 const unsigned char* s=*p;
 if(*s<0x80){(*p)++;return *s;}
 if((*s&0xE0)==0xC0&&(s[1]&0xC0)==0x80){uint32_t c=((uint32_t)(s[0]&31)<<6)|(s[1]&63);*p+=2;return c>=0x80?c:0xFFFD;}
 if((*s&0xF0)==0xE0&&(s[1]&0xC0)==0x80&&(s[2]&0xC0)==0x80){uint32_t c=((uint32_t)(s[0]&15)<<12)|((uint32_t)(s[1]&63)<<6)|(s[2]&63);*p+=3;return c>=0x800&&!(c>=0xD800&&c<=0xDFFF)?c:0xFFFD;}
 if((*s&0xF8)==0xF0&&(s[1]&0xC0)==0x80&&(s[2]&0xC0)==0x80&&(s[3]&0xC0)==0x80){uint32_t c=((uint32_t)(s[0]&7)<<18)|((uint32_t)(s[1]&63)<<12)|((uint32_t)(s[2]&63)<<6)|(s[3]&63);*p+=4;return c>=0x10000&&c<=0x10FFFF?c:0xFFFD;}
 (*p)++;return 0xFFFD;
}
static uint32_t fold(uint32_t c){if(c>='A'&&c<='Z')return c+32;if(c>=0x0410&&c<=0x042F)return c+32;if(c==0x0401)return 0x0451;return c;}
static int contains(const char* h,const char* n){
 if(!n||!*n)return 1;
 const unsigned char* start=(const unsigned char*)h;
 while(*start){const unsigned char* a=start;const unsigned char* b=(const unsigned char*)n;
  while(*a&&*b){const unsigned char* aa=a;const unsigned char* bb=b;if(fold(decode(&aa))!=fold(decode(&bb)))break;a=aa;b=bb;}
  if(!*b)return 1;
  (void)decode(&start);
 }return 0;
}
unsigned prog_catalog_count(void){return APP_CATALOG_COUNT;}
const prog_catalog_entry_t* prog_catalog_get(unsigned i){return i<APP_CATALOG_COUNT?(const prog_catalog_entry_t*)&APP_CATALOG[i]:0;}
unsigned prog_catalog_search(const char* q,unsigned* out,unsigned cap){
 if(!out||!cap)return 0;
 unsigned n=0;
 for(unsigned i=0;i<APP_CATALOG_COUNT&&n<cap;i++){const app_catalog_entry_t* e=&APP_CATALOG[i];
  if(contains(e->name,q)||contains(e->title,q)||contains(e->category,q)||contains(e->description,q))out[n++]=i;
 }return n;
}
