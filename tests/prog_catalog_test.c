#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prog_catalog.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\\n", __LINE__, #c); exit(1); } } while (0)
int main(void) {
 CHECK(prog_catalog_count()==2);
 const prog_catalog_entry_t* a=prog_catalog_get(0); const prog_catalog_entry_t* b=prog_catalog_get(1);
 CHECK(a&&!strcmp(a->name,"paint")&&!strcmp(a->title,"Рисование"));
 CHECK(b&&!strcmp(b->name,"drvman")&&!strcmp(b->category,"Система"));
 CHECK(!prog_catalog_get(2));
 unsigned out[8];
 CHECK(prog_catalog_search("PAINT",out,8)==1&&out[0]==0);
 CHECK(prog_catalog_search("рисов",out,8)==1&&out[0]==0);
 CHECK(prog_catalog_search("РИСОВАНИЕ",out,8)==1&&out[0]==0);
 CHECK(prog_catalog_search("сИСтЕма",out,8)==1&&out[0]==1);
 CHECK(prog_catalog_search("нет такого",out,8)==0);
 CHECK(prog_catalog_search("",out,8)==2&&out[0]==0&&out[1]==1);
 CHECK(prog_catalog_search("paint",0,8)==0&&prog_catalog_search("paint",out,0)==0);
 puts("app catalog: allowlist, generated membership, UTF-8 case-folded search and bounds passed");return 0;
}
