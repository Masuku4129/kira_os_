#ifndef KIRA_MIND_BUILD_GRANT_H
#define KIRA_MIND_BUILD_GRANT_H
/* Решение о собранном образе: разрешение человека или подписи, проверка перед запуском, темп заданий, правило Разума.
   Эта часть работает в ЯДРЕ (консольная команда build, buildcmd.c) и на хосте; компилятора в ней нет (он - в mind_build.c,
   процесс kcc без прав). Разрешения хранятся не в песочнице, где пишет сборщик, а в таблице ядра (mbj_grants_*): процесс kcc
   не может ни выдать, ни подделать разрешение. Описание модели - в mind_build.h и docs/BUILDER_PLAN.md (этап D). */
#include "mind_build.h"
#include "kdrv_sig.h"
#include "mind.h"

/* Разрешения. buf - память под образ (cap байт); обе функции читают образ заново и сверяют с записью. */
int mbj_authorize_human(const mbj_env_t* env, const char* name, const char* typed_hex, void* buf, size_t cap);
int mbj_authorize_signed(const mbj_env_t* env, const char* name, const void* sig, size_t siglen,
                         const kdrv_sig_key_t* keys, uint32_t nkeys, void* buf, size_t cap);
/* Для запускающего: 0 только если образ в buf (*len байт) совпал с записью и разрешением и снова проходит elf_parse. */
int mbj_check_grant(const mbj_env_t* env, const char* name, void* buf, size_t cap, size_t* len);
/* Запись «ожидает» и образ без решения: для списка. 0 - запись и образ целы (*rec, *len); иначе MBJ_E_NOREC/TAMPER/ELF_INVALID. */
int mbj_inspect(const mbj_env_t* env, const char* name, void* buf, size_t cap, mbj_rec_t* rec, size_t* len);
/* Какое решение записано: 0 нет, MBJ_GRANT_HUMAN, MBJ_GRANT_SIGNED (хеш в разрешении не сверяется - это делает mbj_check_grant). */
int mbj_grant_kind(const mbj_env_t* env, const char* name);

/* Таблица разрешений ядра: имя (без суффикса) -> разрешение. Живёт в памяти, после перезагрузки решение нужно дать заново. */
#define MBJ_GRANT_SLOTS 8
typedef struct { char name[48]; mbj_grant_t g; uint8_t used; } mbj_grant_slot_t;
typedef struct { mbj_grant_slot_t s[MBJ_GRANT_SLOTS]; } mbj_grants_t;
int  mbj_grants_put(mbj_grants_t* t, const char* side_name, const void* data, size_t len);      /* side_name = "ИМЯ.ok"; 0 или -1 (таблица полна, формат) */
int64_t mbj_grants_get(const mbj_grants_t* t, const char* side_name, void* buf, size_t cap);    /* длина или -1 */
void mbj_grants_drop(mbj_grants_t* t, const char* name);

/* Темп заданий: не чаще одного за min_gap_s и не больше max_pending ожидающих решения. */
typedef struct { uint32_t last_s, pending, min_gap_s, max_pending; uint8_t any; } mbj_rate_t;
int  mbj_rate_allow(const mbj_rate_t* r, uint32_t now_s);
void mbj_rate_note_built(mbj_rate_t* r, uint32_t now_s);
void mbj_rate_note_proposed(mbj_rate_t* r, uint32_t now_s);     /* предложение показано человеку: отсчёт паузы, ожидающих не прибавляет */
void mbj_rate_note_decided(mbj_rate_t* r);

/* Правило Разума: предлагает задание «калибровочный зонд», только если система спокойна. 1 - предложено (в *job), 0 - нет.
   Само по себе ничего не собирает и не меняет. Параметры зависят от скорости процессора из замера. */
int mind_build_propose(const mind_t* s, const mind_sample_t* in, uint32_t now_s, const mbj_rate_t* r, mbj_job_t* job);

#endif
