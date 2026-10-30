#ifndef KIRA_TRUST_H
#define KIRA_TRUST_H
#include <stdint.h>

/* Манифест доверенных программ: имя -> SHA-256 образа. Его составляет сборка (tools/kfstool manifest) и вшивает в ядро,
   поэтому подмена файла на диске не может подменить и эталон. */
typedef struct { const char* name; uint8_t sha[32]; } trust_entry_t;

typedef enum {
    TRUST_OK = 0,         /* имя в манифесте, хеш совпал */
    TRUST_TAMPERED = 1,   /* имя в манифесте, но содержимое другое */
    TRUST_UNKNOWN = 2     /* имени в манифесте нет */
} trust_verdict_t;

trust_verdict_t trust_check(const trust_entry_t* tbl, unsigned n, const char* name, const uint8_t digest[32]);

#endif
