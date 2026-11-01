#ifndef KIRA_IPFRAG_H
#define KIRA_IPFRAG_H
#include <stdint.h>

/* Сборка фрагментированных датаграмм IP (RFC 791, RFC 815 - блоки по 8 байт; для IPv6 - RFC 8200 п. 4.5 и RFC 5722:
   перекрытие отбрасывает всю датаграмму; терпится только точный повтор: то же место, те же байты). Чистый код: время передаётся числом (тики), память - в самой структуре,
   потому проверяется на хосте. Один поток (поток «net»).

   Пределы памяти: IPF_SLOTS датаграмм одновременно, каждая до IPF_MAX байт данных (после заголовка IP); не больше
   IPF_PER_SRC незаконченных датаграмм от одного отправителя; незаконченное живёт не дольше ttl тиков.
   Когда все места заняты, новая датаграмма вытесняет самую старую только если той не меньше min_age тиков, иначе
   фрагмент отбрасывается (поток чужих первых фрагментов не вытесняет настоящую сборку, идущую миллисекунды). */
#define IPF_SLOTS   4
#define IPF_MAX     8192
#define IPF_PER_SRC 2
#define IPF_BLOCKS  (IPF_MAX / 8)

typedef struct {
    uint8_t  fam;                       /* 4 или 6 */
    uint8_t  src[16], dst[16];          /* IPv4 - в первых 4 байтах */
    uint32_t id;                        /* IPv4: 16 бит, IPv6: 32 бита */
    uint8_t  proto;
} ipf_key_t;

typedef struct {
    int       used;
    ipf_key_t key;
    uint64_t  born;                     /* когда пришёл первый фрагмент */
    uint32_t  total;                    /* длина всей датаграммы (известна по последнему фрагменту), 0 - ещё нет */
    uint32_t  blocks;                   /* принято блоков по 8 байт */
    uint32_t  maxend;                   /* конец самого дальнего принятого байта */
    int       aux;                      /* значение, которое несёт первый фрагмент (IPv6: следующий заголовок) */
    uint8_t   map[IPF_BLOCKS / 8];      /* какие блоки приняты */
    uint8_t   first[IPF_BLOCKS / 8];    /* с каких блоков начинались фрагменты */
    uint8_t   last[IPF_BLOCKS / 8];     /* на каких блоках фрагменты кончались */
    uint8_t   data[IPF_MAX];
} ipf_slot_t;

typedef struct {
    uint64_t ttl, min_age;
    ipf_slot_t slot[IPF_SLOTS];
    /* счётчики (для команды net и тестов) */
    uint64_t assembled, expired, overlap, toobig, malformed, nospace, dup;
} ipfrag_t;

void ipfrag_init(ipfrag_t* t, uint64_t ttl_ticks, uint64_t min_age_ticks);

typedef struct { const uint8_t* data; uint32_t len; int aux; } ipf_out_t;

/* Добавить фрагмент: off - смещение данных в датаграмме (байты, кратно 8), more - есть ещё фрагменты (MF).
   aux - значение, которое несёт только фрагмент со смещением 0 (IPv6: поле «следующий заголовок» заголовка фрагмента);
   у IPv4 не нужно (0). Результат: 1 - датаграмма собрана: out->data/len (действительны до следующего вызова ipfrag_*),
   out->aux - значение первого фрагмента; место уже свободно; 0 - принято или повтор, ждём остальное;
   -1 - фрагмент (и вся его датаграмма) отброшены. */
int  ipfrag_add(ipfrag_t* t, uint64_t now, const ipf_key_t* k, uint32_t off, int more,
                const uint8_t* d, uint32_t n, int aux, ipf_out_t* out);
void ipfrag_expire(ipfrag_t* t, uint64_t now);     /* убрать незаконченное старше ttl */
unsigned ipfrag_pending(const ipfrag_t* t);        /* сколько датаграмм сейчас собирается */
#endif
