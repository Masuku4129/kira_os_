#ifndef IMMUNE_H
#define IMMUNE_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    IMMUNE_SELF = 0,     // дайджест есть в базе "своих"
    IMMUNE_UNKNOWN = 1,  // не свой и сигнатур антител не найдено -> карантин
    IMMUNE_HOSTILE = 2   // сработало антитело -> карантин
} immune_verdict_t;

typedef struct {
    uint8_t digest[32];
    uint32_t size;
    immune_verdict_t verdict;
    char reason[32];
} immune_quarantine_entry_t;

// SHA-256 (потоковый интерфейс и одним вызовом)
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    uint32_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* c);
void sha256_update(sha256_ctx_t* c, const void* data, size_t len);
void sha256_final(sha256_ctx_t* c, uint8_t out[32]);
void sha256(const void* data, size_t len, uint8_t out[32]);

// Снимает эталонный дайджест кода и констант ядра (.rodata + .text).
void immune_init(void);

// Повторно считает дайджест и сравнивает с эталоном. 0 - цело, -1 - изменено.
int immune_verify_self(void);

// Регистрирует дайджест как "свой". 0 - успех, -1 - таблица полна.
int immune_register_self(const uint8_t digest[32]);

// Добавляет антитело (сигнатуру байтов, до 16 байт). 0 - успех, -1 - таблица полна.
int immune_add_antibody(const char* name, const uint8_t* pattern, uint32_t len);

// Классификация буфера: своё / неизвестное / враждебное. Чужое и враждебное
// заносится в карантин.
immune_verdict_t immune_classify(const void* buf, size_t size);

// "Фагоцитоз": затирает память буфера нулями (по volatile, оптимизатор не уберёт).
void immune_phagocytose(void* buf, size_t size);

// Восстановление записи карантина с диска (при загрузке).
void immune_quarantine_import(const uint8_t digest[32], uint32_t size, uint32_t verdict, const char* reason);

uint32_t immune_quarantine_count(void);
const immune_quarantine_entry_t* immune_quarantine_get(uint32_t index);
uint32_t immune_antibody_count(void);

#endif
