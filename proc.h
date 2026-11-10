#ifndef PROC_H
#define PROC_H
#include <stdint.h>
#include <stddef.h>

/* Процесс = своё адресное пространство (PML4) + поток планировщика в Ring 3.
   Код и данные процесса видны только ему: нижняя половина PML4 (ядро) без бита USER. */

void proc_init(unsigned cpus, uint64_t ram_kb);   /* лимит одновременных программ зависит от ядер и ОЗУ */
unsigned proc_limit(void);
void proc_user_window(uint64_t* lo, uint64_t* hi, uint32_t* max_pages);   /* окно сегментов программы и предел страниц - те же числа, что проверяет spawn (elf_parse) */
/* Запускает программу из образа ELF (проверяется elf_parse). Решение «доверять ли» принимает progs.c до вызова. pid или -1. */
int  proc_spawn_elf(const char* name, const void* image, size_t len);
/* То же с правом читать один файл grant (путь VFS): его получает программа просмотра из Проводника. */
int  proc_spawn_elf_grant(const char* name, const void* image, size_t len, const char* grant);
/* Драйвер Ring 3 (kdrv.c): права и привязка к устройству задаёт менеджер драйверов, а не таблица caps.c. 0.. - pid, -1 - отказ. */
int  proc_spawn_driver(const char* name, const void* image, size_t len, uint32_t caps, uint32_t slot, const char* args);
uint32_t proc_current_drv(void);         /* номер привязки + 1; 0 - не драйвер */
int  proc_map_phys(uint64_t va, uint64_t pa, uint32_t pages, uint64_t flags);   /* страницы в пространство текущего процесса (под proc_space_lock); VMM_SHARED добавляется */
void proc_unmap_pages(uint64_t va, uint32_t pages);                              /* снять и сбросить TLB (под proc_space_lock) */
uint32_t proc_current_caps(void);        /* права текущего процесса (caps.h); 0 - не процесс */
int  proc_cap_denied(uint32_t cap, uint64_t nr);   /* записывает отказ в журнал (раз на процесс); возвращает KE_PERM */
const char* proc_current_grant(void);   /* "" - права нет */
int  proc_spawn_elf_ex(const char* name, const void* image, size_t len, const char* grant, const char* args);
/* Запуск недоверенного/внешне подписанного образа: capabilities всегда нулевые, независимо от имени. */
int  proc_spawn_elf_unprivileged(const char* name, const void* image, size_t len, const char* args);
const char* proc_current_args(void);    /* аргументы запуска ("" - нет) */
void proc_exit(int code) __attribute__((noreturn));
int  proc_stack_fault(uint64_t cr2, uint64_t err);   /* #PF программы в резерве стека: 1 - стек вырос */
void proc_fault(uint64_t vector, uint64_t err, uint64_t rip, uint64_t cr2) __attribute__((noreturn));
/* SYS_JIT (право CAP_JIT): окно кода JIT процесса - до JIT_MAX_PAGES страниц. Страница живёт в двух состояниях: RW-NX (пишется) и
   R-X (запечатана навсегда: обратно в запись не открывается, пока процесс жив). Страницы W+X одновременно не бывает никогда. */
enum { JIT_ALLOC = 1, JIT_SEAL = 2 };
#define JIT_MAX_PAGES 256u
int64_t proc_jit(uint64_t op, uint64_t a, uint64_t b);   /* ALLOC(страниц) -> адрес; SEAL(адрес, страниц) -> 0; отрицательный код при отказе */
int64_t proc_heap_grow(uint64_t pages);                             /* SYS_HEAP: адрес нового куска кучи или отрицательный код */
int  proc_in_user_thread(void);                                     /* текущий поток - процесс? */
uint32_t proc_current_pid(void);
const char* proc_current_name(void);
uint32_t proc_alive_count(void);
void proc_dump(void);

// Потоки и объекты ядра (системные вызовы). kobj_t - kobj.h.
struct kobj;
void    proc_thread_exit(int code) __attribute__((noreturn));   /* уходит текущий поток (последний - процесс целиком) */
int64_t proc_thread_create(uint64_t rip, uint64_t stack_top, uint64_t arg);   /* id потока или отрицательный код */
int     proc_dying(void);                                     /* процесс текущего потока завершается */
void    proc_return_check(void);                              /* выход из ядра в Ring 3: процесс завершается - поток уходит */
int64_t proc_handle_add(struct kobj* o);                      /* забирает ссылку; дескриптор или код */
struct kobj* proc_handle_get(int h, uint32_t type);           /* +1 ссылка (отпустить kobj_release) */
int64_t proc_handle_close(int h);
int64_t proc_shm_map(int h);                                  /* адрес в программе */
int64_t proc_shm_unmap(uint64_t va);
void    proc_space_lock(void);                                /* изменения адресного пространства текущего процесса */
void    proc_space_unlock(void);

// Для Разума: снимок процессов, счётчик аварийных завершений, остановка по номеру, осторожный рост куч.
typedef struct { uint32_t pid; char name[16]; uint32_t threads; uint32_t heap_kb; uint64_t syscalls; uint32_t dying; } proc_stat_t;
unsigned proc_stats(proc_stat_t* out, unsigned max);
uint32_t proc_faults_total(void);
int      proc_kill(uint32_t pid);
void     proc_set_heap_restrict(int level);
void     proc_count_syscall(void);                            /* из обработчика системного вызова */

int      proc_stack_prepare(uint64_t va, uint64_t len);       /* буфер вызова на невыросшей части стека */

#endif
