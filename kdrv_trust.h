#ifndef KIRA_KDRV_TRUST_H
#define KIRA_KDRV_TRUST_H
/* Только открытые ключи. Закрытый ключ выпуска остаётся офлайн и вне Git/ISO/CI.
   Ротация: новый ключ добавляется второй записью массива (и строкой #define KDRV_TRUST_KEY_ID_NEXT "...", чтобы tools/kdrvsign.py
   мог его выбрать), у старого заполняется not_after - дата, с которой старый ключ больше не принимается (проверяется по времени,
   защищённому от отката, kdrv_state.h). Компрометация ключа - не ротация: ключ отзывают (flags или drv revoke), а не ждут not_after.
   Сейчас в доверии ровно один рабочий ключ: второго не создавали, чтобы не хранить его закрытую часть без процедуры выпуска. */
#include "kdrv_sig.h"

#if defined(KDRV_TEST_TRUST) && !defined(KIRA_TEST_BUILD)
#error "KDRV_TEST_TRUST is restricted to isolated test builds"
#endif

#ifdef KDRV_TEST_TRUST
#define KDRV_TRUST_KEY_ID "TEST-ED25519-001"
#define KDRV_TRUST_KEY_ID_NEXT "TEST-ED25519-002"   /* второй тестовый ключ (RFC 8032, вектор 2): ротация и отзыв проверяются без пересборки */
static const kdrv_sig_key_t kdrv_trust_keys[] = {
    {
        KDRV_TRUST_KEY_ID,
        {
            0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
            0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
            0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
            0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a
        },
        0,  /* flags: KDRV_SIG_KEY_REVOKED выставляется здесь только обновлением ядра; отзыв без обновления - drv revoke */
        0   /* not_after: 0 - без срока; при ротации старому ключу ставят дату замены (Unix UTC) */
    },
    {
        KDRV_TRUST_KEY_ID_NEXT,
        {
            0x3d,0x40,0x17,0xc3,0xe8,0x43,0x89,0x5a,
            0x92,0xb7,0x0a,0xa7,0x4d,0x1b,0x7e,0xbc,
            0x9c,0x98,0x2c,0xcf,0x2e,0xc4,0x96,0x8c,
            0xc0,0xcd,0x55,0xf1,0x2a,0xf4,0x66,0x0c
        },
        0,
        0
    }
};
#else
#define KDRV_TRUST_KEY_ID "aa85df60c87d3666"
static const kdrv_sig_key_t kdrv_trust_keys[] = {
    {
        KDRV_TRUST_KEY_ID,
        {
            0x86,0xef,0x20,0x0e,0x9a,0x9f,0xab,0xb3,
            0x58,0x1d,0xea,0xc8,0xa3,0x1e,0xac,0x91,
            0x21,0x9c,0x44,0xb8,0x4b,0xb4,0x4c,0xb8,
            0x05,0x83,0x0b,0x47,0xcf,0xf5,0x54,0xe8
        },
        0,  /* flags: KDRV_SIG_KEY_REVOKED выставляется здесь только обновлением ядра; отзыв без обновления - drv revoke */
        0   /* not_after: 0 - без срока; при ротации старому ключу ставят дату замены (Unix UTC) */
    }
};
#endif /* KDRV_TEST_TRUST */

#define KDRV_TRUST_KEY_COUNT ((uint32_t)(sizeof kdrv_trust_keys / sizeof kdrv_trust_keys[0]))

#endif
