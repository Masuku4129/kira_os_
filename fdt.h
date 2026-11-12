#ifndef FDT_H
#define FDT_H

#include <stdint.h>

/* Дерево устройств (Flattened Device Tree, DTB): так загрузчик или прошивка ARM (и RISC-V, и ряд x86-плат)
   описывают железо - память, ядра, контроллер прерываний, таймер, порты. Ядро не знает адресов заранее: всё
   берёт отсюда. Чистый разбор без выделения памяти; блоб только читается, все проверки границ - на каждом шаге. */

#define FDT_MAGIC 0xD00DFEEDu

int      fdt_valid(const void* fdt, uint32_t max_len);      /* 1 - заголовок и границы блоков разумны */
int      fdt_tree_valid(const void* fdt, uint32_t max_len);                   /* 1 - весь токен-поток структуры корректен */
uint32_t fdt_total_size(const void* fdt);
/* Firmware memory-reservation map (64-bit address/size pairs); index order is preserved. */
unsigned fdt_reserve_count(const void* fdt, uint32_t max_len);
int fdt_reserve(const void* fdt, uint32_t max_len, unsigned index, uint64_t* base, uint64_t* size);
/* Узлы задаются смещением в блоке структуры (>= 0); -1 - нет. */
int      fdt_root(const void* fdt);
int      fdt_next_node(const void* fdt, int node, int* depth);   /* обход всего дерева по порядку */
const char* fdt_node_name(const void* fdt, int node);            /* «uart@9000000» */
const void* fdt_prop(const void* fdt, int node, const char* name, uint32_t* len);
int fdt_has_bool(const void* fdt, int node, const char* name);
int      fdt_compatible(const void* fdt, int node, const char* compat);   /* 1 - узел совместим */
int      fdt_find_compatible(const void* fdt, int after, const char* compat);  /* следующий после after (-1 - с начала) */
int      fdt_find_path(const void* fdt, const char* path);       /* «/cpus», «/chosen»; имя без @адреса тоже подходит */
int      fdt_parent(const void* fdt, int node);
/* reg[idx] узла в числах родителя (#address-cells / #size-cells). 0 - есть. */
int      fdt_reg(const void* fdt, int node, unsigned idx, uint64_t* addr, uint64_t* size);
uint32_t fdt_u32(const void* p);                                  /* большой порядок байт -> число */
int      fdt_prop_u32(const void* fdt, int node, const char* name, uint32_t* out);
/* Узлы-дети с device_type = type (например, ядра «cpu» в /cpus). */
unsigned fdt_count_type(const void* fdt, int parent, const char* type);

/* Узел включён: status нет, «okay» или «ok». */
int fdt_node_enabled(const void* fdt, int node);
int fdt_find_enabled_compatible(const void* fdt, int after, const char* compat);
/* Кадровый буфер от загрузчика/прошивки (compatible = «simple-framebuffer», включённый): 0 - найден и формат
   32 бита x8r8g8b8/a8r8g8b8 (как у ядра); -1 - нет узла; -2 - неполный узел; -3 - другой формат (в format). */
typedef struct { uint64_t base, size; uint32_t width, height, stride; char format[16]; } fdt_fb_t;
int fdt_simplefb(const void* fdt, fdt_fb_t* fb);

#endif
