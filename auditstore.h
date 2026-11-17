#ifndef KIRA_AUDITSTORE_H
#define KIRA_AUDITSTORE_H
/* Журнал аудита на диске KiraFS: сегменты файлов, ротация без разрыва цепочки, восстановление после обрыва питания, проверка с диска.
   Без ядра, кроме kfs_* (хост-тест tests/audit_test.c гоняет его на настоящей kfs.c с обрывами питания, как tests/kfs_crash_test.c).
   Файлы: /var/audit.log - текущий сегмент; /var/audit.NNNNNNNNNN.log - закрытые (NNNNNNNNNN - номер первой записи); /var/audit.ckpt - последняя подписанная
   точка (отдельный свидетель: обрезка журнала без правки этого файла видна). KiraFS не умеет дописывать, но заменяет файл атомарно: текущий сегмент
   (до 41 КБ) переписывается целиком, закрытые не меняются никогда. Программам файлы недоступны: песочница программы - /data/<имя> (fsrv.c). */
#include "auditcore.h"

#define AUD_DIR       "/var"
#define AUD_ACTIVE    "/var/audit.log"
#define AUD_CKPTFILE  "/var/audit.ckpt"
#define AUD_MAX_SEGS  64                /* закрытых сегментов не больше: дальше самые старые удаляются с записью AUD_K_PRUNE (docs/AUDIT.md, решение о заполнении) */
#define AUD_SEGMAX    256               /* сколько закрытых сегментов запоминаем при загрузке */
#define AUD_SEGBYTES  (AUD_HDR_SIZE + AUD_REC_SIZE * AUD_SEG_RECS)

typedef struct {
    uint8_t  pub[32];
    uint8_t  cur[AUD_SEGBYTES];          /* текущий сегмент: заголовок и записи */
    size_t   cur_len;
    uint64_t cur_first;                  /* номер первой записи текущего сегмента */
    uint8_t  closed[AUD_SEGBYTES];       /* полностью набранный сегмент, ещё не записанный в закрытый файл */
    size_t   closed_len;                 /* 0 - нет */
    uint64_t closed_first;
    uint8_t  tmp[AUD_SEGBYTES];          /* рабочие буферы чтения (загрузка, проверка, экспорт) */
    uint8_t  tmp2[AUD_SEGBYTES];
    uint8_t  tail_head[32];              /* хэш последней принятой записи */
    uint64_t ckpt_file_seq;              /* точка в /var/audit.ckpt */
    aud_rec_t last_ckpt;                 /* последняя принятая точка (якорь следующего сегмента) */
    int      have_ckpt;
    uint64_t next_seq;                   /* следующая запись, которую ждёт append */
    uint64_t flushed_next;               /* следующая запись, которой ещё нет на диске */
    uint8_t  flushed_head[32];           /* хэш последней записи на диске */
    int      active_dirty;               /* текущий сегмент изменён и не записан */
    uint64_t segs[AUD_SEGMAX];           /* первые номера закрытых файлов по возрастанию */
    unsigned nsegs;
    unsigned io_errors;                  /* ошибок записи на диск подряд (0 после успеха) */
    uint64_t loaded_files;
} aud_store_t;

/* Читает и проверяет всё, что есть на диске, и подготавливает продолжение цепочки. 0 - диск доступен (результат проверки - в v), -1 - нет /var или ошибка чтения.
   Нарушение не мешает продолжить: цепочка продолжается от последней записи, фактически лежащей на диске, а нарушение остаётся в журнале навсегда. */
int  aud_store_load(aud_store_t* s, const uint8_t pub[32], aud_vstate_t* v, aud_chain_t* chain);
/* Принимает запечатанную (и подписанную, если точка) запись. 0 - принята; -1 - некуда: закрытый сегмент ещё не записан на диск. Записи идут строго по номерам. */
int  aud_store_append(aud_store_t* s, const aud_rec_t* r);
int  aud_store_room(const aud_store_t* s);   /* 1 - append примет запись; 0 - сначала aud_store_flush (закрытый сегмент ждёт записи) */
/* Пишет на диск всё принятое. 0 - всё на диске; -1 - ошибка (состояние цело, повторить позже). */
int  aud_store_flush(aud_store_t* s);
/* Полная проверка с диска: файлы, цепочка, подписи, /var/audit.ckpt, а также что на диске есть всё, что уже записано (flushed_*). Результат - в v. */
int  aud_store_verify(aud_store_t* s, aud_vstate_t* v);
/* Читает запись seq с диска (для экспорта). 0 - есть. Использует s->tmp. */
int  aud_store_read_rec(aud_store_t* s, uint64_t seq, aud_rec_t* out);
/* Самый старый закрытый сегмент лишний (сегментов больше AUD_MAX_SEGS)? Тогда в *upto - последний номер записи в нём. */
int  aud_store_prune_due(const aud_store_t* s, uint64_t* upto);
/* Удаляет закрытые сегменты, целиком не новее upto (после того, как запись AUD_K_PRUNE уже на диске). Возвращает, сколько удалено. */
unsigned aud_store_prune(aud_store_t* s, uint64_t upto);
#endif
