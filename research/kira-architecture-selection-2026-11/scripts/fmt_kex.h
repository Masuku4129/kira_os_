#ifndef FMT_KEX_H
#define FMT_KEX_H
/* ИССЛЕДОВАТЕЛЬСКИЙ ПРОТОТИП (не production): контейнер KEX1 = заголовок + таблица сегментов + таблица перемещений + данные, SHA-256 в заголовке.
   Нужен только для честного сравнения с elf_parse() (elf.c): размер файла, строки кода разбора, число проверок границ, время, устойчивость к фаззингу.
   Данные сегментов лежат подряд БЕЗ выравнивания по странице (загрузчик KIRA и так копирует: proc.c map_fresh). Результат - тот же elf_info_t. */
#include "../../../elf.h"
#define KEX_MAGIC 0x3158454Bu              /* "KEX1" */
#define KEX_HDR 96u
#define KEX_SEG 24u
#define KEX_REL 8u
#define KEX_MAX_RELOC 65536u
enum { KEX_OK = 0, KEX_ERR_SHORT = -1, KEX_ERR_MAGIC = -2, KEX_ERR_FORMAT = -3, KEX_ERR_SEGMENT = -4, KEX_ERR_ENTRY = -5, KEX_ERR_LIMIT = -6, KEX_ERR_RELOC = -7, KEX_ERR_HASH = -8 };
/* заголовок (LE): 0 magic u32 | 4 ver u16 (=1) | 6 hdr_size u16 (=96) | 8 total u32 (== len) | 12 arch u16 (62/183) | 14 nseg u16 | 16 entry u64 |
   24 seg_off u32 (=96) | 28 nrel u32 | 32 rel_off u32 | 36 flags u32 (=0) | 40..63 резерв (нули) | 64 sha256 u8[32] (по файлу с обнулённым полем).
   сегмент: vaddr u64 | file_off u32 | file_size u32 | mem_size u32 | flags u32 (1=X, 2=W). перемещение: off u32 (смещение слова от vaddr сегмента 0; 8 байт внутри файловой части какого-либо сегмента) | type u32 (=1 RELATIVE). */
int kex_parse(const void* image, size_t len, uint64_t win_lo, uint64_t win_hi, uint32_t max_pages, elf_info_t* out);
int kex_verify_hash(const void* image, size_t len);       /* после kex_parse: SHA-256 совпадает с заголовком */
#endif
