#include "vmm.h"
#include "uaccess.h"
#include "exceptions.h"
#include "uart.h"
#include "display.h"
#include "proc.h"
#include "crashdump.h"
#include "earlycon.h"
#include "spinlock.h"
#include "smp.h"
#include "sched.h"
#include "percpu.h"
#include "corefault.h"
#include "irqmode.h"
#include "apic.h"

static const char* const exception_names[32] = {
    "Divide Error (#DE)",
    "Debug (#DB)",
    "Non-Maskable Interrupt (NMI)",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "BOUND Range Exceeded (#BR)",
    "Invalid Opcode (#UD)",
    "Device Not Available (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Invalid TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack-Segment Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Reserved (15)",
    "x87 Floating-Point Error (#MF)",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved (22)", "Reserved (23)", "Reserved (24)", "Reserved (25)",
    "Reserved (26)", "Reserved (27)",
    "Hypervisor Injection (#HV)",
    "VMM Communication (#VC)",
    "Security Exception (#SX)",
    "Reserved (31)"
};

static void print_page_fault_info(uint64_t err_code, uint64_t cr2) {
    uart_print("[KIRA FATAL] Fault address (CR2): ");
    uart_print_hex(cr2);
    uart_print("\n[KIRA FATAL] Cause: ");
    uart_print((err_code & 1) ? "protection violation" : "page not present");
    uart_print((err_code & 2) ? ", write" : ", read");
    uart_print((err_code & 4) ? ", user mode" : ", kernel mode");
    if (err_code & 8)  uart_print(", reserved bit set");
    if (err_code & 16) uart_print(", instruction fetch");
    uart_print("\n");
}

static void dump_out(const char* s) { uart_print(s); }

static uint64_t read_cr(int n) {
    uint64_t v = 0;
    switch (n) {
    case 0: __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); break;
    case 3: __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); break;
    default: __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); break;
    }
    return v;
}

static const char* not_isolated_why;

// Живые ядра, кроме этого. *irq_apic - первое из них, до которого IOAPIC может доставить прерывание
// (без переназначения прерываний поле адресата IOAPIC - 8 бит); 0xFFFFFFFF - такого нет.
static unsigned other_live_cores(const percpu_t* me, uint32_t* irq_apic) {
    unsigned n = 0;
    *irq_apic = 0xFFFFFFFFu;
    for (unsigned i = 0; i < percpu_count(); i++) {
        const percpu_t* p = &percpu_table[i];
        if (p == me || !p->sched_on || !p->online || p->failed) continue;
        if (*irq_apic == 0xFFFFFFFFu && p->apic_id <= 254) *irq_apic = p->apic_id;
        n++;
    }
    return n;
}

static void core_log(const char* tag, uint64_t vector, uint64_t rip, const char* thread, const char* tail) {
    uart_print("[KIRA CORE "); uart_print_dec(this_cpu()->cpu_id); uart_print("] ");
    uart_print(tag); uart_print(" #"); uart_print_dec(vector);
    uart_print(" at RIP "); uart_print_hex(rip);
    uart_print(", thread '"); uart_print(thread); uart_print("'. "); uart_print(tail); uart_print("\n");
}

// Сбой в режиме ядра: если его можно изолировать, гибнет виновный поток (или отказавшее ядро выходит из работы),
// система живёт. Возвращается только когда изолировать нельзя - тогда обычная авария всей системы.
static void corefault_handle(uint64_t vector, uint64_t err, uint64_t rip, uint64_t cr2, const uint64_t* frame) {
    percpu_t* c = this_cpu();
    thread_t* t = c->sched_on ? (thread_t*)sched_current() : 0;
    uint32_t new_apic = 0;
    unsigned others = other_live_cores(c, &new_apic);     /* до инициализатора: порядок вычисления его полей не задан */
    corefault_input_t in = {
        .vector = (unsigned)vector, .locks_held = c->locks_held, .irq_depth = c->irq_depth, .in_fault = c->in_fault,
        .mutexes_held = t ? t->mutexes_held : 0, .have_thread = t != 0, .service = t ? (t->flags & THREAD_F_SERVICE) != 0 : 0,
        .other_cores = others,
        .irq_move_ok = irqmode_is_apic() ? (irqmode_target() != c->apic_id || new_apic != 0xFFFFFFFFu) : (c->cpu_id != 0),
    };
    corefault_action_t act = corefault_decide(&in);
    c->in_fault = 1;
    c->fail_vector = (uint32_t)vector;
    c->fail_rip = rip;
    if (act == COREFAULT_PANIC) { not_isolated_why = corefault_why_panic(&in); return; }

    if (act == COREFAULT_KILL_THREAD) {
        core_log("kernel-mode fault", vector, rip, t->name, "Thread terminated, the core and the system keep running.");
        /* Для разбора плавающих сбоев: код ошибки, CR2, RSP и слова с вершины стека (при переходе по адресу 0 первое слово - адрес возврата). */
        uint64_t sp = frame[20], tmp;
        uart_print("[KIRA CORE] fault detail: error "); uart_print_hex(err); uart_print(", CR2 "); uart_print_hex(cr2);
        uart_print(", RSP "); uart_print_hex(sp); uart_print(", RBP "); uart_print_hex(frame[8]);
        uart_print(", stack:");
        for (unsigned i = 0; i < 8 && sp >= 0x1000 && !(sp & 7) && vmm_translate(sp + 8ull * i, &tmp) == 0; i++) {   /* до конца отображённого стека */
            uart_print(" "); uart_print_hex(((const uint64_t*)(uintptr_t)sp)[i]);
        }
        uart_print("\n");
        c->in_fault = 0;
        __asm__ volatile ("sti");
        if (proc_in_user_thread()) proc_exit(-1);
        sched_exit();
    }

    // COREFAULT_FAIL_CORE: машинная проверка. Ядро выходит из планирования, его прерывания переезжают.
    core_log("machine check", vector, rip, t->name, "This core is taken out of service; the others continue.");
    unsigned orphans = sched_core_fail();
    smp_timekeeper_failed(c->cpu_id);
    apic_timer_mask_local();
    int moved = irqmode_core_failed(c->apic_id, new_apic);
    uart_print("[KIRA CORE "); uart_print_dec(c->cpu_id); uart_print("] out of service: ");
    uart_print_dec(orphans); uart_print(" pinned thread(s) ended, ");
    uart_print_dec(moved > 0 ? (uint64_t)moved : 0); uart_print(" device IRQ line(s) moved to APIC ID ");
    uart_print_dec(new_apic); uart_print(".\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void exception_handler_main(uint64_t vector, uint64_t err_code, uint64_t rip, uint64_t cr2, uint64_t cs, const uint64_t* frame) {
    // NMI от ядра, остановившего систему при аварии: молча встаём, без второго дампа.
    if (vector == 2 && smp_halting()) {
        percpu_t* me = this_cpu();                 /* где стояло ядро: по этому отчёт об аварии показывает место зависания */
        me->halt_rip = rip;
        me->halt_rbp = (cs & 3) ? 0 : frame[8];    /* кадр: r15..rax, RBP - девятый */
        __atomic_store_n(&me->halted, 1, __ATOMIC_RELEASE);
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // Страница программы исчезла во время копирования ядром (другой поток снял отображение): вызов вернёт ошибку.
    if (vector == 14 && (cs & 3) == 0 && uaccess_fixup((uint64_t*)(uintptr_t)frame, rip)) return;

    // Исключение в Ring 3 - ошибка процесса, а не ядра: убиваем процесс, ядро живёт дальше.
    if ((cs & 3) == 3 && proc_in_user_thread()) {
        if (vector == 14 && proc_stack_fault(cr2, err_code)) return;      /* стек программы вырос - продолжить */
        proc_fault(vector, err_code, rip, cr2);
    }

    __asm__ volatile ("cli");
    if ((cs & 3) == 0 && vector == 13 && rip >= 0x100000 && rip < (1ULL << 30) &&
        ((const uint8_t*)(uintptr_t)rip)[0] == 0x48 && ((const uint8_t*)(uintptr_t)rip)[1] == 0xCF) {
        /* iretq отказал: что именно он пытался загрузить (код ошибки #GP - селектор-виновник) */
        const uint64_t* f = (const uint64_t*)(uintptr_t)frame[20];
        uart_print("[KIRA IRET] #GP on iretq, error code (selector) "); uart_print_hex(err_code);
        uart_print(", frame RIP "); uart_print_hex(f[0]); uart_print(" CS "); uart_print_hex(f[1]);
        uart_print(" RFLAGS "); uart_print_hex(f[2]); uart_print(" RSP "); uart_print_hex(f[3]); uart_print(" SS "); uart_print_hex(f[4]);
        uart_print(", this CPU "); uart_print_dec(this_cpu()->cpu_id); uart_print("\n");
    }
    if ((cs & 3) == 0 && vector != 2 && vector != 8 && vector < 32) corefault_handle(vector, err_code, rip, cr2, frame);
    smp_halt_others();  // остальные ядра встают (второе падающее ядро встанет молча)
    spin_bust_locks();  // замок мог держать упавший код: аварийный вывод не должен на нём зависнуть
    earlycon_alert();   // если экран ещё в режиме ранней консоли - дальше красным
    uart_print("\n==================================================\n");
    uart_print("[KIRA FATAL] CPU exception, vector ");
    uart_print_dec(vector);
    uart_print("\n[KIRA FATAL] Type: ");
    if (vector < 32) {
        uart_print(exception_names[vector]);
    } else {
        uart_print("Unhandled interrupt (no handler installed)");
    }
    if (not_isolated_why) { uart_print("\n[KIRA FATAL] Not isolated: "); uart_print(not_isolated_why); }
    uart_print("\n[KIRA FATAL] RIP: ");
    uart_print_hex(rip);
    uart_print("\n[KIRA FATAL] Error code: ");
    uart_print_hex(err_code);
    uart_print("\n");

    if (vector == 14) {
        print_page_fault_info(err_code, cr2);
    }

    {
        // Стек читаем только если это стек ядра: identity-карта (первый ГБ, не нулевая страница) или отображённая
        // часть ячейки стека потока.
        uint64_t rsp = frame[20];
        int kst = vmm_is_kstack(rsp) && !vmm_kstack_guard_hit(rsp) && !vmm_kstack_guard_hit(rsp + 63) && vmm_is_kstack(rsp + 63);
        const uint64_t* stack = ((cs & 3) == 0 && (rsp & 7) == 0 && (kst || (rsp >= 0x1000 && rsp + 64 <= (1ULL << 30))))
                                    ? (const uint64_t*)(uintptr_t)rsp : 0;
        if ((cs & 3) == 0 && (vmm_kstack_guard_hit(rsp) || ((vector == 14 || vector == 8) && vmm_kstack_guard_hit(cr2))))
            uart_print("[KIRA FATAL] Kernel thread stack overflow: the guard area below the stack was hit.\n");
        crash_regs_t cr = { read_cr(0), cr2, read_cr(3), read_cr(4) };
        crashdump_print(frame, &cr, stack, dump_out);
    }
    uart_print("[KIRA FATAL] System halted.\n");
    uart_print("==================================================\n");

    __asm__ volatile ("cli");
    uart_flush_all();
    display_panic_flush();   // показать то же на экране
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
