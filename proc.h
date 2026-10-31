#ifndef PROC_H
#define PROC_H
#include <stdint.h>
#include <stddef.h>

/* Процесс = своё адресное пространство (PML4) + поток планировщика в Ring 3.
   Код и данные процесса видны только ему: нижняя половина PML4 (ядро) без бита USER. */

void proc_init(unsigned cpus, uint64_t ram_kb);   /* лимит одновременных программ зависит от ядер и ОЗУ */
unsigned proc_limit(void);
/* Запускает программу из образа ELF (проверяется elf_parse). Решение «доверять ли» принимает progs.c до вызова. pid или -1. */
int  proc_spawn_elf(const char* name, const void* image, size_t len);
/* То же с правом читать один файл grant (путь VFS): его получает программа просмотра из Проводника. */
int  proc_spawn_elf_grant(const char* name, const void* image, size_t len, const char* grant);
const char* proc_current_grant(void);   /* "" - права нет */
void proc_exit(int code) __attribute__((noreturn));
int  proc_stack_fault(uint64_t cr2, uint64_t err);   /* #PF программы в резерве стека: 1 - стек вырос */
void proc_fault(uint64_t vector, uint64_t err, uint64_t rip, uint64_t cr2) __attribute__((noreturn));
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
