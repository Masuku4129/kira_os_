#ifndef KIRA_PROG_CATALOG_H
#define KIRA_PROG_CATALOG_H
#include <stdint.h>
typedef struct { const char* name; const char* title; const char* category; const char* description; } prog_catalog_entry_t;
unsigned prog_catalog_count(void);
const prog_catalog_entry_t* prog_catalog_get(unsigned index);
unsigned prog_catalog_search(const char* query, unsigned* indices, unsigned capacity);
#endif
