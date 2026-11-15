/* «Печать системы»: манифест /etc/system.cbor со SHA-256 каждого файла /bin и /etc и корнем дерева Меркла (RFC 6962).
   Манифест пишет tools/kfstool seal при сборке образа, команда ядра verify сверяет с ним весь образ: ловит порчу носителя
   и случайные изменения любого файла (иммунная система проверяет только программы из списка внутри ядра).
   Подпись издателя - отдельный файл /etc/system.sig (конверт kdrv_sig_t, домен KDRV_SIG_DOMAIN_SEAL, ключи kdrv_trust_keys, закрытый ключ офлайн):
   `tools/kdrvsign.py --domain seal` над байтами /etc/system.cbor, затем `kfstool образ put /etc/system.sig файл`. Без подписи печать защищает от порчи,
   но не от того, кто может переписать весь диск; проверку подписи делает kdrv_seal_sig_check (kdrv.c).
   Формат (детерминированный CBOR): {"n": число файлов, "v": 1, "root": байты[32], "files": [[путь, размер, sha256[32]], ...]},
   файлы отсортированы по пути; лист дерева = путь, 0x00, sha256 файла, размер (8 байт, little-endian). Чистый модуль: без ядра. */
#ifndef KIRA_SYSSEAL_H
#define KIRA_SYSSEAL_H
#include <stdint.h>
#include <stddef.h>

#define SEAL_PATH       "/etc/system.cbor"
#define SEAL_NAME_MAX   96
#define SEAL_MAX_FILES  1024

typedef struct { char path[SEAL_NAME_MAX]; uint64_t size; uint8_t hash[32]; } seal_entry;

void seal_leaf(const seal_entry* e, uint8_t out[32]);
/* Строит манифест из записей (уже отсортированных по пути, без повторов). Возвращает длину или 0, если не поместилось/путь слишком длинный. */
size_t seal_build(const seal_entry* e, size_t n, uint8_t* out, size_t cap);

typedef struct {
    uint32_t files, ok, changed, missing;
    int format;                 /* 0 - манифест разобран; иначе код ошибки cbor или -100 (неверная структура) */
    int root_ok;                /* корень в манифесте совпал с пересчитанным по записям манифеста */
    uint8_t root[32];           /* корень из манифеста */
} seal_report;

/* Читает один файл образа: размер и SHA-256. 0 - есть, иначе файла нет или не читается. */
typedef int (*seal_hash_fn)(void* ctx, const char* path, uint64_t* size, uint8_t hash[32]);
/* Разбирает манифест и сверяет каждый файл. leaves - память под SEAL_MAX_FILES * 32 байт. bad (необязательно) вызывается для каждого
   расхождения: why = "изменён" или "нет файла". */
int seal_check(const uint8_t* m, size_t n, seal_hash_fn fn, void* ctx, uint8_t (*leaves)[32],
               void (*bad)(void* ctx, const char* path, const char* why), seal_report* rep);
#endif
