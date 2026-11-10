#ifndef KIRA_DRVGET_CORE_H
#define KIRA_DRVGET_CORE_H
/* drvget: чистая логика без системных вызовов (разбор рецепта и адресов, проверка исходников и лицензии, SHA-256 набора).
   Один и тот же код работает в программе Ring 3 / EL0 и в хост-тесте tests/drvget_test.c. Входу не доверяют. */
#include <stdint.h>
#include <stddef.h>
#include "../kdrv_format.h"

#define DG_FILES      40
#define DG_FILE_NAME  26                    /* имя файла в рецепте: «s-ИМЯ-файл» должно уместиться в 40 знаков песочницы */
#define DG_URL_MAX    200
#define DG_RECIPE_MAX 16384
#define DG_SRC_MAX    (400u * 1024u)        /* один исходный файл */

typedef struct { char host[96]; unsigned port; char path[DG_URL_MAX]; } dg_url_t;

typedef struct {
    char name[KDRV_NAME_MAX + 1];
    uint32_t version;
    char license[24];                       /* заявленная лицензия: из списка dg_license_ok */
    uint32_t caps;                          /* запрошенные права (CAP_* драйвера) */
    uint32_t nmatch;
    kdrv_match_t match[KDRV_MATCH_MAX];
    char main_file[DG_FILE_NAME + 1];
    uint32_t nfile;
    struct { char name[DG_FILE_NAME + 1]; char url[DG_URL_MAX]; char sha[65]; } file[DG_FILES];   /* sha - привязка хеша из рецепта или пусто */
} dg_recipe_t;

int dg_license_ok(const char* id);          /* supported SPDX IDs plus project LicenseRef-Greenman */
int dg_license_matches(const char* declared, const char* detected); /* exact recipe/source license match */
int dg_url_parse(const char* s, dg_url_t* u, const char** err);          /* только https:// ; 0 - готово */
int dg_url_join(const dg_url_t* base, const char* rel, char* out, size_t cap, const char** err);   /* относительный путь или полный адрес https -> адрес */
int dg_parse_recipe(const char* text, uint32_t len, dg_recipe_t* r, const char** err);          /* 0 - готово */
/* Проверка исходного файла: текст (не бинарник Windows/ELF/архив), лицензия из списка, нет GPL. lic - найденная лицензия.
   0 - можно; иначе err объясняет отказ. */
int dg_scan_source(const char* fname, const uint8_t* d, uint32_t len, char lic[24], const char** err);
/* Имя файла из рецепта: «.c» или «.h», буквы, цифры, «.», «-», «_» ; Windows-драйверы и ELF-модули по расширению не пройдут. */
int dg_file_name_ok(const char* n);

typedef struct { void* ctx[16]; } dg_hash_t;   /* непрозрачно: sha256_ctx_t (размер проверяется в drvget_core.c) */
void dg_set_begin(dg_hash_t* h);
void dg_set_add(dg_hash_t* h, const char* name, const uint8_t* d, uint32_t len);   /* имя, нуль, длина (8 байт), данные */
void dg_set_end(dg_hash_t* h, uint8_t out[32]);
void dg_hex(const uint8_t* d, int n, char* out);                 /* 2n знаков и нуль */
int  dg_hex_prefix(const char* typed, const uint8_t sha[32]);    /* 1 - typed (>= 8 знаков hex) - начало хеша */
int  dg_hex_parse(const char* s, uint8_t* out, int n);           /* 1 - ровно 2n знаков */

#define DG_INDEX_MAX 4096u
#define DG_INDEX_ENTRIES 32u
typedef struct {
    uint16_t vendor, device;
    char name[12];
    char url[DG_URL_MAX];
    char recipe_sha[65];
    uint8_t publisher_required;
} dg_index_entry_t;
int dg_parse_index(const char *text, uint32_t len, dg_index_entry_t *out, uint32_t cap, uint32_t *count, const char **err);
int dg_index_find_pci(const dg_index_entry_t *entries, uint32_t count, uint16_t vendor, uint16_t device);
int dg_index_find_name(const dg_index_entry_t *entries, uint32_t count, const char *name);

#endif
