#ifndef KIRA_AUDITCORE_H
#define KIRA_AUDITCORE_H
/* Журнал аудита: формат записи, цепочка SHA-256, подписанные контрольные точки Ed25519, формат файла сегмента и проверка.
   Чистый модуль: ни ядра, ни диска (проверяется хост-тестом tests/audit_test.c, ту же проверку делает tools/auditverify.py
   независимо от ядра). Подробно - docs/AUDIT.md. */
#include <stdint.h>
#include <stddef.h>
#include "auditev.h"

#define AUD_REC_SIZE   160
#define AUD_HDR_SIZE   256
#define AUD_SEG_RECS   256              /* записей в сегменте; последняя запись каждого сегмента (seq кратен 256) - контрольная точка */
#define AUD_CKPT_EVERY 16               /* не больше столько записей без подписи (и не дольше AUD_CKPT_IDLE_S секунд) */
#define AUD_CKPT_IDLE_S 5

/* Запись (160 байт, little-endian, без заполнения). Хэш записи = SHA-256 всех её байт (у контрольной точки поле sig при хэшировании
   обнуляется: подпись ставится уже после того, как запись заняла место в цепочке). prev - хэш предыдущей записи (у первой записи нули). */
typedef struct __attribute__((packed)) {
    uint64_t seq;                       /*   0 порядковый номер с 1 без пропусков */
    uint64_t mono_ms;                   /*   8 монотонные часы (мс от начала счёта времени ядра; со сбросом при каждой загрузке) */
    uint64_t real_s;                    /*  16 часы реального времени, секунды UTC от 1970 (0 - часов нет) */
    uint16_t kind;                      /*  24 AUD_K_* */
    uint8_t  src;                       /*  26 AUD_SRC_* */
    uint8_t  res;                       /*  27 AUD_R_* */
    uint32_t pid;                       /*  28 процесс (0 - ядро) */
    uint32_t domain;                    /*  32 домен приложений (DOM_*) */
    uint32_t arg;                       /*  36 число по смыслу вида */
    char     subject[24];               /*  40 кто/что, UTF-8, с нулём */
    char     detail[56];                /*  64 подробности, UTF-8, с нулём */
    uint32_t boot;                      /* 120 номер запуска (растёт на 1 при каждой загрузке) */
    uint32_t arg2;                      /* 124 резерв: ноль */
    uint8_t  prev[32];                  /* 128 хэш предыдущей записи */
} aud_rec_t;

/* Контрольная точка (kind = AUD_K_CKPT): та же длина и те же первые 40 и последние 32 байта, середина другая. */
typedef struct __attribute__((packed)) {
    uint64_t seq, mono_ms, real_s;
    uint16_t kind; uint8_t src, res;
    uint32_t pid, domain;
    uint32_t arg;                       /*  36 сколько записей подписано (= seq - 1: все записи до этой) */
    uint8_t  sig[64];                   /*  40 Ed25519 над "KIRA-AUDIT-CKPT1" || хэш записи */
    uint8_t  keyid[16];                 /* 104 первые 16 байт SHA-256 открытого ключа устройства */
    uint32_t boot, arg2;
    uint8_t  prev[32];
} aud_ckpt_t;

/* Заголовок файла сегмента (256 байт): сегмент проверяется сам по себе, зная только открытый ключ устройства. */
typedef struct __attribute__((packed)) {
    char     magic[8];                  /* "KIRAAUD1" */
    uint32_t version;                   /* 1 */
    uint32_t rec_size;                  /* 160 */
    uint64_t first_seq;                 /* номер первой записи сегмента: 256*k + 1 */
    uint8_t  pub[32];                   /* открытый ключ устройства */
    uint8_t  anchor[32];                /* хэш записи first_seq - 1 (нули у первого сегмента) */
    uint8_t  pad[8];
    uint8_t  anchor_ckpt[AUD_REC_SIZE]; /* эта запись (контрольная точка, подписанная) - ею подтверждено начало сегмента (нули у первого) */
} aud_hdr_t;

_Static_assert(sizeof(aud_rec_t) == AUD_REC_SIZE, "aud_rec_t");
_Static_assert(sizeof(aud_ckpt_t) == AUD_REC_SIZE, "aud_ckpt_t");
_Static_assert(sizeof(aud_hdr_t) == AUD_HDR_SIZE, "aud_hdr_t");

const char* aud_kind_name(unsigned kind);
const char* aud_src_name(unsigned src);
const char* aud_res_name(unsigned res);

void aud_hash(const aud_rec_t* r, uint8_t out[32]);          /* хэш записи для цепочки */
void aud_keyid(const uint8_t pub[32], uint8_t id[16]);

/* Состояние цепочки на стороне пишущего. */
typedef struct { uint64_t next_seq; uint8_t head[32]; } aud_chain_t;
void aud_chain_init(aud_chain_t* c, uint64_t next_seq, const uint8_t head[32]);   /* head 0 - начало цепочки */
/* Ставит seq и prev, считает хэш, сдвигает цепочку. Записи с кратным AUD_SEG_RECS номером должны быть контрольными точками: следит вызывающий (aud_next_is_ckpt). */
void aud_seal(aud_chain_t* c, aud_rec_t* r);
int  aud_next_is_ckpt(const aud_chain_t* c);                 /* 1 - следующий номер кратен AUD_SEG_RECS: нужна контрольная точка */
void aud_ckpt_fill(aud_rec_t* r, const uint8_t pub[32]);     /* поля контрольной точки (kind, keyid); seq/prev - aud_seal */
int  aud_ckpt_sign(aud_rec_t* r, const uint8_t seed[32]);    /* подпись над запечатанной записью; 0 - готово */
int  aud_ckpt_check(const aud_rec_t* r, const uint8_t pub[32]);   /* 0 - подпись верна */

void aud_hdr_make(aud_hdr_t* h, uint64_t first_seq, const uint8_t pub[32], const aud_rec_t* anchor_ckpt);   /* anchor_ckpt 0 - первый сегмент */

/* Проверка. Нарушения (код результата): */
enum {
    AUD_V_OK = 0,
    AUD_V_FORMAT,       /* файл короче заголовка, чужая метка/версия, обрезанная запись */
    AUD_V_KEY,          /* ключ в заголовке или подпись контрольной точки не от ключа устройства */
    AUD_V_ANCHOR,       /* начало сегмента не подтверждено: подписанная точка не та или не стыкуется с прошлым сегментом */
    AUD_V_SEQ,          /* разрыв, повтор или перестановка номеров: удалена, вставлена или переставлена запись */
    AUD_V_CHAIN,        /* prev не совпал с хэшем предыдущей: запись изменена (или удалена/подменена) */
    AUD_V_SIG,          /* подпись контрольной точки неверна */
    AUD_V_TRUNC,        /* журнал короче, чем подтверждено подписанной точкой: хвост отрезан */
    AUD_V_PRUNE         /* самые старые записи пропали без записи AUD_K_PRUNE */
};
const char* aud_verr_name(int code);

typedef struct {
    int      started;
    uint64_t next_seq;           /* какая запись ожидается следующей */
    uint8_t  head[32];           /* хэш последней принятой записи */
    uint64_t first_seq;          /* с какой записи начинается проверенная часть */
    uint64_t count;              /* принято записей */
    uint64_t ckpts;              /* из них проверенных контрольных точек */
    uint64_t last_ckpt_seq;
    uint64_t prune_upto;         /* наибольший номер, удалённый по записи AUD_K_PRUNE */
    uint64_t want_seq;           /* подписанная точка из стороннего источника (/var/audit.ckpt): журнал обязан её содержать */
    uint8_t  want_hash[32];
    int      want_seen;
    int      verify_sigs;        /* 1 - проверять подпись каждой точки (иначе только якорь и последние: см. verify_from) */
    uint64_t sig_from;           /* точки с номером меньше этого подпись не проверяют (быстрая проверка при загрузке) */
    int      code;               /* первый найденный код нарушения; AUD_V_OK - нет */
    uint64_t bad_seq;            /* где: номер записи (для CHAIN - запись, у которой prev не сошёлся; изменена она или предыдущая) */
    uint64_t bad_off;            /* смещение в файле */
    int      bad_file;           /* какой по порядку файл (с 0) */
    int      files;
} aud_vstate_t;

void aud_vstate_init(aud_vstate_t* v);
void aud_vstate_want(aud_vstate_t* v, const aud_rec_t* ckpt);   /* запомнить подписанную точку (после проверки подписи вызывающим) */
/* Проверяет один файл сегмента и продолжает цепочку состояния. Возвращает v->code (первое нарушение остаётся навсегда: проверка не останавливается,
   чтобы состояние можно было продолжить и посчитать записи, но новые нарушения не перезаписывают первое). */
int  aud_verify_file(aud_vstate_t* v, const uint8_t* buf, size_t len, const uint8_t pub[32]);
int  aud_verify_end(aud_vstate_t* v);       /* итог: сверка с подписанной точкой (хвост) и пропавшим началом */

/* Проверка набора записей без заголовков (страница экспорта): внутренняя цепочка, номера, подписи точек. 0 - ок, иначе AUD_V_*; *bad - номер записи. */
int  aud_verify_range(const aud_rec_t* r, size_t n, const uint8_t pub[32], int check_sigs, uint64_t* bad);

/* Текст строки записи для консоли: "#12 +3456ms 2026-11-17T10:00:00Z rcon login FAIL pid=0 dom=0 arg=2 subj detail". Возвращает длину. */
size_t aud_format(const aud_rec_t* r, char* out, size_t cap);
#endif
