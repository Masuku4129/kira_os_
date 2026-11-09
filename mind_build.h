#ifndef KIRA_MIND_BUILD_H
#define KIRA_MIND_BUILD_H
/* Задание на сборку: Разум -> kcc (этап D плана сборщика, docs/BUILDER_PLAN.md).

   Что делает: по шаблону (не произвольному тексту) формирует исходник C, пропускает его через жёсткие квоты и собирает
   встроенным kcc (kcc_compile). Результат - ELF в песочнице (/data/kcc на устройстве) и запись «ожидает решения».
   Чего не делает НИКОГДА: не устанавливает и не запускает собранное. В модуле нет вызова запуска. Файлы модуля:
     mind_build_core.c   - общее (границы параметров, имена, hex, тексты ошибок), без зависимостей;
     mind_build.c        - СБОРКА (шаблон, ворота, kcc); работает в процессе kcc без прав (userland/kcc.c, режим @build) и на хосте;
     mind_build_grant.c  - РЕШЕНИЕ (разрешения, проверка перед запуском, темп, правило Разума); работает в ядре (buildcmd.c) и на хосте.
   Запуск допустим только
   после выдачи разрешения, и это разрешение есть лишь двух видов, оба уже существуют в системе:
     - человек: ввод первых 8+ знаков SHA-256 образа из отчёта (как drvget confirm, dg_hex_prefix);
     - подпись издателя Ed25519 над байтами образа (kdrv_sig_verify, ключи kdrv_trust_keys; закрытый ключ вне системы).
   Разрешение записывается в таблицу ядра (см. mind_build_grant.h) вместе с SHA-256 образа и действует только на те же байты: mbj_check_grant
   перечитывает образ и сверяет хеш в момент запуска (изменённый образ разрешения не имеет).
   Программа, прошедшая проверку, по-прежнему запускается без прав (caps = 0, `runlocal`); задание не умеет просить права.

   Квоты: арена компилятора (память, без роста), размер исходника, число лексем (исходник без препроцессора: '#' запрещён,
   поэтому счёт до компиляции точен), размер ELF и время. Граница: время проверяется ПОСЛЕ сборки (kcc не имеет хука
   прерывания); сверху сборку ограничивают число лексем и размер исходника, а принудительное снятие зависшего процесса -
   дело вызывающего (на устройстве - отдельный процесс kcc под kill). Модуль не потокобезопасен: kcc держит глобальный контекст. */
#include <stddef.h>
#include <stdint.h>

enum { MBJ_T_PROBE = 1, MBJ_T_TABLE = 2 };

typedef struct { uint32_t tmpl; uint32_t p[3]; } mbj_job_t;
/* PROBE: p0 = итераций цикла (1..4194304), p1 = начальное значение. Возвращает контрольную сумму цикла.
   TABLE: p0 = число записей (1..256), p1 = множитель, p2 = модуль (1..65536). Возвращает взвешенную сумму таблицы. */

typedef struct { uint32_t arena_bytes, max_src, max_tokens, max_elf, max_ms; } mbj_quota_t;   /* 0 - значение по умолчанию */
#define MBJ_DEF_ARENA  (1u << 20)
#define MBJ_DEF_SRC    8192u
#define MBJ_DEF_TOKENS 4000u
#define MBJ_DEF_ELF    (128u << 10)
#define MBJ_DEF_MS     500u
#define MBJ_HARD_ARENA (4u << 20)       /* выше этого квоту задать нельзя: ошибка MBJ_E_QUOTA, а не молчаливое урезание */
#define MBJ_HARD_SRC   16384u
#define MBJ_HARD_TOKENS 8000u
#define MBJ_HARD_ELF   (256u << 10)
#define MBJ_HARD_MS    2000u

enum {
    MBJ_OK = 0,
    MBJ_E_ARG = -1,          /* неизвестный шаблон или параметр вне диапазона */
    MBJ_E_QUOTA = -2,        /* квота выше жёсткого предела */
    MBJ_E_SRC_SIZE = -3,
    MBJ_E_SRC_FORBIDDEN = -4,/* '#', asm, байты вне ASCII и т.п. */
    MBJ_E_TOKENS = -5,
    MBJ_E_COMPILE = -6,      /* kcc отверг исходник (текст ошибки в res->err) */
    MBJ_E_ARENA = -7,        /* арена исчерпана */
    MBJ_E_ELF_SIZE = -8,
    MBJ_E_ELF_INVALID = -9,  /* образ не прошёл elf_parse ядра */
    MBJ_E_TIME = -10,
    MBJ_E_IO = -11,
    MBJ_E_NOREC = -12,       /* нет записи или образа */
    MBJ_E_NOGRANT = -13,     /* разрешения нет */
    MBJ_E_TAMPER = -14,      /* образ не совпал с записью или разрешением */
    MBJ_E_CONFIRM = -15,     /* введённый хеш не совпал */
    MBJ_E_SIG = -16,         /* подпись отвергнута (причина - kdrv_sig_strerror) */
    MBJ_E_RATE = -17         /* слишком рано или слишком много ожидающих */
};

typedef struct {
    uint64_t (*now_ms)(void* ud);                         /* монотонные часы, мс */
    int64_t  (*now_utc)(void* ud);                        /* Unix UTC, с (для срока подписи) */
    int      (*put)(void* ud, const char* name, const void* data, size_t len);        /* 0 - записано; имя - [A-Za-z0-9._-] */
    int64_t  (*get)(void* ud, const char* name, void* buf, size_t cap);               /* длина или < 0; больше cap - ошибка */
    void*    ud;
    void*    arena; size_t arena_cap;                       /* память компилятора (расти не может) */
    const char* target;                                    /* "x86_64" или "aarch64" */
    uint64_t win_lo, win_hi; uint32_t max_pages;            /* окно адресов программы для elf_parse */
} mbj_env_t;

typedef struct {
    int      status;
    char     err[200];
    char     name[40];            /* имя образа в песочнице */
    uint8_t  elf_sha[32];
    char     elf_hex[65];         /* показывается человеку для подтверждения */
    size_t   elf_len;
    uint32_t src_len, tokens, ms, arena_used;
} mbj_result_t;

typedef struct { uint32_t magic, version, tmpl, state, elf_len, src_len; uint8_t elf_sha[32], src_sha[32]; } mbj_rec_t;
typedef struct { uint32_t magic, kind, elf_len, reserved; uint8_t elf_sha[32]; } mbj_grant_t;
#define MBJ_REC_MAGIC   0x314A424Du   /* "MBJ1" */
#define MBJ_GRANT_MAGIC 0x31474A4Du   /* "MJG1" */
enum { MBJ_STATE_PENDING = 1 };
enum { MBJ_GRANT_HUMAN = 1, MBJ_GRANT_SIGNED = 2 };

/* Шаблон -> исходник. Возвращает длину или < 0 (MBJ_E_ARG, MBJ_E_SRC_SIZE). Параметры - только числа. */
int mbj_render(const mbj_job_t* job, char* out, size_t cap);
/* Проверка исходника до компиляции: размер, запрещённые конструкции, число лексем. */
int mbj_source_gate(const char* src, size_t len, const mbj_quota_t* q, uint32_t* tokens, char* why, size_t why_cap);
/* Исходник -> ELF -> проверка -> песочница + запись «ожидает». Исходник проходит тот же mbj_source_gate. */
int mbj_build_source(const mbj_env_t* env, const mbj_quota_t* q, uint32_t tmpl, const char* src, size_t len, mbj_result_t* res);
/* Шаблон + параметры -> сборка. Единственный вход, которым пользуется Разум. */
int mbj_build(const mbj_env_t* env, const mbj_quota_t* q, const mbj_job_t* job, mbj_result_t* res);

/* Общее для сборщика и ядра (mind_build_core.c, без зависимостей). */
int  mbj_job_valid(const mbj_job_t* job);                 /* MBJ_OK или MBJ_E_ARG: единственное место, где заданы границы параметров шаблонов */
int  mbj_name_valid(const char* name);                    /* имя образа в песочнице: "mb" + строчные латинские, цифры и '-', 4..32 знака */
void mbj_hex(const uint8_t* d, int n, char* out);         /* n байт -> 2n знаков + NUL */
int  mbj_hex_prefix(const char* typed, const uint8_t sha[32]);   /* 1: typed - от 8 до 64 знаков hex, совпадающих с началом sha (без учёта регистра) */
const char* mbj_strerror(int status);

#endif
