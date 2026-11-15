#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

void syscall_init(void);
void syscall_init_cpu(void);
void syscall_proc_gone(uint32_t pid);   /* вывести незаконченную строку завершившегося процесса */    /* MSR SYSCALL на текущем процессоре (каждый настраивает свои) */
uint64_t syscall_handler(uint64_t syscall_number, uint64_t arg1, uint64_t arg2, uint64_t arg3);


/* Службы, на которые диспетчер передаёт вызовы (сигнатуры совпадают с функциями gfxsrv.h, fsrv.h, net.h, kdrv.h). Все поля обязательны.
   Коды операций net/udp - как NET_SYS_* и NET_UDP_* в net.h (wire.c это проверяет). */
enum { SVC_NET_CONNECT_SA = 8, SVC_NET_PEER = 9 };
enum { SVC_UDP_SENDTO = 4, SVC_UDP_RECVFROM = 5 };
typedef struct {
    int64_t (*gfx_create)(uint32_t pid, const char* title, uint32_t w, uint32_t h);
    int64_t (*gfx_present)(uint32_t pid);
    int64_t (*gfx_event)(uint32_t pid, uint64_t user_ptr);
    int64_t (*fs)(uint64_t user_req_ptr);
    int64_t (*net)(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
    int64_t (*udp)(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
    int64_t (*kdrv)(unsigned nr, uint64_t a1, uint64_t a2, uint64_t a3);
} syscall_services_t;
void syscall_set_services(const syscall_services_t* s);   /* до запуска первой программы (wire_init) */

#endif
