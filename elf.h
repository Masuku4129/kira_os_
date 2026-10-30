#ifndef KIRA_ELF_H
#define KIRA_ELF_H
#include <stdint.h>
#include <stddef.h>

/* Проверка и разбор исполняемого файла ELF64 для программ Ring 3. Принимается только то, что ядро готово отобразить:
   ET_EXEC для x86-64, сегменты PT_LOAD с выровненными по странице адресами в окне пользовательской программы,
   без W+X, без пересечения страниц, точка входа внутри исполняемого сегмента. Ничего не загружает и не доверяет
   полям файла: все смещения и размеры проверяются на выход за границы образа. */

#define ELF_MAX_SEGS 16

typedef struct {
    uint64_t vaddr;          /* выровнен по странице */
    uint64_t file_off;       /* смещение данных в образе (выровнено так же, как vaddr) */
    uint64_t file_size;      /* байт данных в файле */
    uint64_t mem_size;       /* байт в памяти (хвост после file_size - нули) */
    uint8_t write, exec;
} elf_seg_t;

typedef struct {
    uint64_t entry;
    unsigned nsegs;
    elf_seg_t seg[ELF_MAX_SEGS];
    uint32_t pages;          /* всего страниц под сегменты */
} elf_info_t;

enum { ELF_OK = 0, ELF_ERR_SHORT = -1, ELF_ERR_MAGIC = -2, ELF_ERR_FORMAT = -3, ELF_ERR_SEGMENT = -4, ELF_ERR_ENTRY = -5, ELF_ERR_LIMIT = -6 };

/* [win_lo, win_hi) - допустимое окно адресов; max_pages - предел страниц под сегменты. */
int elf_parse(const void* image, size_t len, uint64_t win_lo, uint64_t win_hi, uint32_t max_pages, elf_info_t* out);
const char* elf_strerror(int err);

#endif
