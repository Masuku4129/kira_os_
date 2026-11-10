// Хост-тест политики планировщика: состояния потоков, round-robin, приоритеты,
// сон/пробуждение, idle, завершение/сборка, канарейка стека, кванты.
// Контекстное переключение подменено заглушкой (меняется только current).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "sched.h"
#include "timer.h"
#include "spinlock.h"
#include "percpu.h"

static uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
const void* fpu_initial_state(void) { static uint8_t z[512] __attribute__((aligned(16))); return z; }
void context_switch(uint64_t* old_rsp_slot, uint64_t new_rsp) { (void)old_rsp_slot; (void)new_rsp; }

static jmp_buf escape;
static const char* panic_reason;
void kernel_panic(const char* r) { panic_reason = r; spin_bust_locks(); longjmp(escape, 1); }   // как в ядре: паника отменяет замки
void host_halt(void) { longjmp(escape, 2); }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void dummy(void* a) { (void)a; }

static unsigned long long cpu_ticks[4096]; // индекс - id потока (id растут монотонно)

// Один тик времени: таймер -> sched_tick -> (после EOI) вытеснение.
static void tick(void) {
    fake_ticks++;
    sched_tick();
    sched_irq_exit();
    cpu_ticks[sched_current()->id]++;
}

static const thread_t* find(const char* name) {
    thread_info_t info[256];
    unsigned n = sched_snapshot(info, sched_thread_capacity());
    (void)n;
    return 0;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    (void)find;
    sched_init(1, 65536);
    CHECK(!strcmp(sched_current()->name, "core") && sched_current()->state == THREAD_RUNNING);
    thread_info_t info[256];
    CHECK(sched_snapshot(info, sched_thread_capacity()) == 2);   // core + idle

    // ---- 1. два равных потока делят процессор поровну, core спит ----
    sched_set_quantum(4);
    thread_t* a = sched_create("A", dummy, 0, PRIO_NORMAL);
    thread_t* b = sched_create("B", dummy, 0, PRIO_NORMAL);
    CHECK(a && b && a->id != b->id);
    thread_t* core = (thread_t*)sched_current();
    uint32_t core_id = core->id;
    sched_sleep_ticks(100000);                              // core засыпает -> выбран A или B
    CHECK(sched_current() == a || sched_current() == b);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    uint64_t sw0 = sched_total_switches();
    for (int i = 0; i < 800; i++) tick();
    long long da = (long long)cpu_ticks[a->id], db = (long long)cpu_ticks[b->id];
    printf("fair share: A=%lld B=%lld idle=%llu switches=%llu\n", da, db, cpu_ticks[2], (unsigned long long)(sched_total_switches() - sw0));
    CHECK(da + db == 800);
    CHECK(llabs(da - db) <= 8);                             // отличие не больше двух квантов
    CHECK(sched_total_switches() - sw0 >= 190 && sched_total_switches() - sw0 <= 210); // ~ 800/4

    // ---- 1b. регрессия: critical-поток просыпается КАЖДЫЙ тик (как core), а два
    //          обычных потока всё равно должны делить остаток поровну ----
    {
        thread_t* pulse = sched_create("pulse", dummy, 0, PRIO_CRITICAL);
        CHECK(pulse != 0);
        // pulse просыпается на границе тика, работает мгновенно и засыпает: остаток тика
        // достаётся тому, кого планировщик выберет следом (его и учитываем)
        static unsigned long long share[4096];
        memset(share, 0, sizeof share);
        int pulses = 0;
        for (int i = 0; i < 2000; i++) {
            tick();
            if (sched_current() == pulse) { pulses++; sched_sleep_ticks(1); }
            share[sched_current()->id]++;
        }
        long long pa = (long long)share[a->id], pb = (long long)share[b->id];
        printf("fair share with a pulsing critical thread: A=%lld B=%lld pulses=%d\n", pa, pb, pulses);
        CHECK(pulses > 900);
        CHECK(pa > 300 && pb > 300);
        CHECK(llabs(pa - pb) <= pa / 10 + 8);            // разница не больше ~10%
        pulse->state = THREAD_UNUSED; free(pulse->stack_base); pulse->stack_base = 0;
        if (sched_current() == pulse) tick();
    }

    // ---- 2. квант влияет на число переключений ----
    sched_set_quantum(8);
    sw0 = sched_total_switches();
    for (int i = 0; i < 800; i++) tick();
    uint64_t sw8 = sched_total_switches() - sw0;
    printf("quantum 8: switches=%llu\n", (unsigned long long)sw8);
    CHECK(sw8 >= 90 && sw8 <= 110);                         // вдвое меньше, чем при 4
    sched_set_quantum(0); CHECK(sched_get_quantum() == 1);   // границы
    sched_set_quantum(999); CHECK(sched_get_quantum() == 20 * TIMER_PER_10MS);
    sched_set_quantum(4);

    // ---- 3. core (critical) просыпается ровно в срок и вытесняет обычные потоки ----
    CHECK(core->sleep_index != UINT32_MAX);                  // срок core зарегистрирован в min-heap
    sched_set_wake_tick(core, fake_ticks + 10);             // перенос существующего узла на новый срок
    CHECK(core->sleep_index != UINT32_MAX);
    uint64_t wake_at = core->wake_tick;
    uint64_t got = 0;
    for (int i = 0; i < 50 && !got; i++) { tick(); if (sched_current() == core) got = fake_ticks; }
    printf("core wake: due %llu, got %llu\n", (unsigned long long)wake_at, (unsigned long long)got);
    CHECK(got == wake_at && core->sleep_index == UINT32_MAX); // извлечён из heap ровно при пробуждении
    sched_sleep_ticks(100000);
    CHECK(sched_current() == a || sched_current() == b);

    // ---- 4. idle: все спят -> работает idle, пробуждение мгновенно уходит с idle ----
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    thread_t* first = (thread_t*)sched_current();
    sched_sleep_ticks(30);                                  // текущий (A или B) засыпает на 30 тиков
    CHECK(first->sleep_index != UINT32_MAX);
    thread_t* second = (thread_t*)sched_current();
    CHECK(second != first && (second == a || second == b));
    sched_sleep_ticks(40);                                  // второй - на 40
    CHECK(second->sleep_index != UINT32_MAX);
    CHECK(!strcmp(sched_current()->name, "idle"));
    for (int i = 0; i < 29; i++) { tick(); CHECK(!strcmp(sched_current()->name, "idle")); }
    tick();                                                 // 30-й тик: first проснулся
    CHECK(sched_current() == first && first->sleep_index == UINT32_MAX);
    int back = 0;
    for (int i = 0; i < 12 && !back; i++) { tick(); if (sched_current() == second) back = 1; }
    CHECK(back && second->sleep_index == UINT32_MAX);       // second проснулся на 40-м, heap membership снят
    CHECK(cpu_ticks[2] >= 29);                              // idle (id 2) честно учтён

    // ---- 4b. раннее пробуждение удаляет произвольный узел из таймерной кучи ----
    thread_t* early = sched_create("early", dummy, 0, PRIO_CRITICAL);
    CHECK(early != 0);
    for (int i = 0; i < 32 && sched_current() != early; i++) tick();
    CHECK(sched_current() == early);
    sched_sleep_ticks(100);
    CHECK(early->state == THREAD_SLEEPING && early->sleep_index != UINT32_MAX);
    sched_wake_waiter(early);
    CHECK(early->state == THREAD_READY && early->sleep_index == UINT32_MAX);
    for (int i = 0; i < 32 && sched_current() != early; i++) tick();
    CHECK(sched_current() == early);
    for (int i = 0; i < 110; i++) tick();                    // старый срок не должен повторно разбудить поток
    CHECK(early->state != THREAD_SLEEPING && early->sleep_index == UINT32_MAX);

    // ---- 5. периодический critical-поток не страдает от нагрузки ----
    sched_sleep_ticks(1000);                                // core убираем
    thread_t* p = sched_create("periodic", dummy, 0, PRIO_CRITICAL);
    CHECK(p != 0);
    unsigned long long max_lat = 0; int runs = 0;
    uint64_t due = 0;
    // модель: periodic при получении CPU работает 1 тик и засыпает на 25 тиков
    sched_sleep_ticks(0);
    for (int i = 0; i < 500; i++) {
        tick();
        if (sched_current() == p) {
            if (due) { unsigned long long lat = fake_ticks - due; if (lat > max_lat) max_lat = lat; }
            runs++;
            sched_sleep_ticks(25);
            due = fake_ticks + 25;
        }
    }
    printf("periodic runs=%d max wake latency=%llu ticks\n", runs, max_lat);
    p->state = THREAD_UNUSED; free(p->stack_base); p->stack_base = 0;  // убираем periodic
    if (sched_current() == p) { /* не должно случиться: он только что заснул */ }
    CHECK(runs >= 18);
    CHECK(max_lat <= 1);

    // ---- 6. завершение потока, сборка, повторное использование слота ----
    unsigned before = sched_snapshot(info, sched_thread_capacity());
    thread_t* x = sched_create("x", dummy, 0, PRIO_NORMAL);
    CHECK(x != 0);
    while (sched_current() != x) tick();
    if (setjmp(escape) == 0) sched_exit();                  // host_halt вернёт сюда
    CHECK(x->state == THREAD_ZOMBIE);
    CHECK(sched_current() != x);
    CHECK(sched_reap() == 1);
    CHECK(x->state == THREAD_UNUSED);
    CHECK(sched_snapshot(info, sched_thread_capacity()) == before);
    thread_t* y = sched_create("y", dummy, 0, PRIO_NORMAL);
    CHECK(y == x);                                          // слот переиспользован
    y->state = THREAD_UNUSED; free(y->stack_base); y->stack_base = 0;

    // ---- 7. таблица потоков ограничена ----
    int created = 0; thread_t* spare[256];
    for (int i = 0; i < (int)sched_thread_capacity() + 4; i++) { thread_t* t = sched_create("s", dummy, 0, PRIO_IDLE); if (t) spare[created++] = t; }
    printf("extra threads created: %d\n", created);
    CHECK(sched_create("over", dummy, 0, PRIO_NORMAL) == 0);
    for (int i = 0; i < created; i++) { spare[i]->state = THREAD_UNUSED; free(spare[i]->stack_base); spare[i]->stack_base = 0; }

    // ---- 7b. несколько ядер: маска ядер, свой текущий и холостой поток у каждого, поток не живёт на двух ядрах ----
    {
        extern unsigned host_cpu;
        percpu_alloc(0); percpu_alloc(1);                          // таблица ядер: 0 (загрузочное) и 1
        host_cpu = 1;
        CHECK(sched_attach_cpu() == 0);
        thread_t* idle1 = (thread_t*)sched_current();
        CHECK(idle1 && idle1->prio == PRIO_IDLE && cm_is64(&idle1->affinity, 2) && idle1->on_cpu == 1);
        host_cpu = 0;
        CHECK(sched_current() != idle1);                          // у ядра 0 свой текущий поток

        CHECK(sched_create_on("nomask", dummy, 0, PRIO_NORMAL, cm_none()) == 0);   // пустая маска бессмысленна
        thread_t* only1 = sched_create_on("only1", dummy, 0, PRIO_NORMAL, cm_from64(2));
        CHECK(only1 != 0 && cm_is64(&only1->affinity, 2));
        thread_t* plain = sched_create("plain", dummy, 0, PRIO_NORMAL);
        CHECK(plain && cm_is_all(&plain->affinity));     // ядра равнозначны: по умолчанию поток может идти на любое

        memset(cpu_ticks, 0, sizeof cpu_ticks);
        for (int i = 0; i < 60; i++) tick();
        CHECK(cpu_ticks[only1->id] == 0);                          // ядро 0 не берёт чужой поток
        CHECK(cpu_ticks[plain->id] > 0);

        host_cpu = 1;
        for (int i = 0; i < 5 && sched_current() != only1; i++) tick();
        CHECK(sched_current() == only1);                           // ядро 1 берёт свой поток
        CHECK(only1->on_cpu == 1 && only1->state == THREAD_RUNNING);
        host_cpu = 0;
        CHECK(sched_current() != only1);

        // Поток, ещё не ушедший со своего стека (on_cpu), другое ядро взять не может, даже если он уже "готов".
        thread_t* both = sched_create_on("both", dummy, 0, PRIO_CRITICAL, cm_from64(3));
        CHECK(both != 0);
        both->on_cpu = 1; both->state = THREAD_READY;
        memset(cpu_ticks, 0, sizeof cpu_ticks);
        for (int i = 0; i < 40; i++) tick();
        CHECK(cpu_ticks[both->id] == 0);
        both->on_cpu = 0;                                          // стек сохранён: теперь можно
        for (int i = 0; i < 40; i++) tick();
        CHECK(cpu_ticks[both->id] > 0);

        uint64_t idle_c = 0, total_c = 0, sw = 0;
        CHECK(sched_cpu_stats(1, &idle_c, &total_c, &sw) == 0 && total_c > 0 && sw >= 1);
        CHECK(sched_cpu_stats(5, &idle_c, &total_c, &sw) != 0);    // ядро не подключено

        // Общая очередь: ядро 0 занято критическим потоком, обычный поток с маской «любое ядро» берёт ядро 1.
        thread_t* roam = sched_create("roam", dummy, 0, PRIO_NORMAL);
        CHECK(roam != 0);
        host_cpu = 1;
        // ядро 1 сейчас исполняет only1 (обычный приоритет): дадим ему свободную очередь, only1 уступает по кванту
        for (int i = 0; i < 4 * 25 && sched_current() != roam; i++) tick();
        CHECK(sched_current() == roam && roam->last_cpu == 1);    // поток переехал на другое ядро без всякой миграции
        host_cpu = 0;
        CHECK(sched_current() != roam);
        {
            thread_info_t inf[256]; unsigned n = sched_snapshot(inf, sched_thread_capacity()); int seen = 0;
            for (unsigned i = 0; i < n; i++) if (!strcmp(inf[i].name, "roam")) { seen = 1; CHECK(inf[i].cpu == 1); }
            CHECK(seen);
        }
        CHECK(sched_cpu_count() >= 2);
        roam->state = THREAD_ZOMBIE;
        host_cpu = 1; roam->on_cpu = 1;
        for (int i = 0; i < 3 && sched_current() == roam; i++) sched_yield();
        CHECK(sched_current() != roam);
        host_cpu = 0;

        // уборка: потоки завершаются, ядро 0 уходит с них, затем сборка
        host_cpu = 0;
        only1->on_cpu = 0;                                         // ядро 1 в тесте больше не работает
        only1->state = plain->state = both->state = THREAD_ZOMBIE;
        for (int i = 0; i < 4 && (sched_current() == plain || sched_current() == both); i++) sched_yield();   // как sched_exit: уходит с процессора
        CHECK(sched_current() != plain && sched_current() != both);
        CHECK(sched_reap() == 4);   // only1, plain, both и roam
        {   // ядро 1 в тесте больше не работает: поток, который оно держало, возвращается в общую очередь
            thread_t* held = percpu_table[1].current;
            if (held && held != idle1 && held->state == THREAD_RUNNING) { held->state = THREAD_BLOCKED; held->on_cpu = 0; sched_wake(held); }
        }
        idle1->state = THREAD_UNUSED; idle1->on_cpu = 0;
        percpu_table[1].sched_on = 0; percpu_table[1].current = 0; percpu_table[1].idle = 0;
    }

    // ---- 7c. отказ ядра: виновный поток гибнет, привязанные только к нему - тоже, остальные живут на других ядрах ----
    {
        extern unsigned host_cpu;
        percpu_alloc(1);
        host_cpu = 1;
        CHECK(sched_attach_cpu() == 0);
        thread_t* idle1 = (thread_t*)sched_current();
        host_cpu = 0;
        thread_t* pin1 = sched_create_on("pin1", dummy, 0, PRIO_NORMAL, cm_from64(2));
        thread_t* both = sched_create_on("both01", dummy, 0, PRIO_NORMAL, cm_from64(3));
        thread_t* svc = sched_create("svc", dummy, 0, PRIO_NORMAL);
        CHECK(pin1 && both && svc);
        CHECK((svc->flags & THREAD_F_SERVICE) && !(pin1->flags & THREAD_F_SERVICE));   // сбой в служебном потоке - авария
        host_cpu = 1;
        for (int i = 0; i < 6 * 25 && sched_current() != pin1; i++) tick();
        CHECK(sched_current() == pin1);
        CHECK(sched_thread_alive(pin1->id));
        unsigned orphans = sched_core_fail();                       // «отказ» ядра 1, пока оно исполняет pin1
        host_cpu = 0;
        CHECK(orphans == 0);                                        // привязанный pin1 - сам виновник, остальных привязанных нет
        CHECK(pin1->state == THREAD_ZOMBIE && pin1->on_cpu == 1);   // стек виновного не освобождается из-под ядра
        CHECK(!sched_thread_alive(pin1->id));
        CHECK(percpu_table[1].failed == 1 && percpu_table[1].sched_on == 0 && percpu_table[1].online == 0);
        CHECK(cm_is64(&both->affinity, 1) && both->state != THREAD_ZOMBIE);   // «оба ядра» стало «только 0»
        CHECK(cm_empty(&idle1->affinity));
        memset(cpu_ticks, 0, sizeof cpu_ticks);
        for (int i = 0; i < 80; i++) tick();
        CHECK(cpu_ticks[both->id] > 0);                             // выжившее ядро продолжает работу с общими потоками
        CHECK(cpu_ticks[pin1->id] == 0);
        // уборка
        pin1->on_cpu = 0;
        both->state = svc->state = THREAD_ZOMBIE;
        for (int i = 0; i < 4 && (sched_current() == both || sched_current() == svc); i++) sched_yield();
        CHECK(sched_reap() == 3);
        idle1->state = THREAD_UNUSED; idle1->on_cpu = 0;
        percpu_table[1].sched_on = 0; percpu_table[1].current = 0; percpu_table[1].idle = 0; percpu_table[1].failed = 0;
    }

    // ---- 8. канарейка стека ----
    thread_t* v = sched_create("victim", dummy, 0, PRIO_NORMAL);
    while (sched_current() != v) tick();
    *(uint64_t*)v->stack_base = 0;                          // "переполнение стека"
    panic_reason = 0;
    if (setjmp(escape) == 0) { for (int i = 0; i < 4 * 25; i++) tick(); }   // квант 20 тиков: проверка канарейки при первом вытеснении
    CHECK(panic_reason && strstr(panic_reason, "stack overflow"));

    // ---- 9. realtime вытесняет critical и не отдаёт процессор ----
    v->state = THREAD_UNUSED; free(v->stack_base); v->stack_base = 0;
    uint32_t cur_before = sched_current()->id; (void)cur_before;
    thread_t* rt = sched_create("rt", dummy, 0, PRIO_REALTIME);
    CHECK(rt != 0);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    for (int i = 0; i < 100; i++) tick();
    CHECK(cpu_ticks[rt->id] >= 99);                         // всё время у rt, core/periodic не получают ничего
    CHECK(cpu_ticks[core_id] == 0);

    // ---- 10. вес и ограничение доли процессора (их задаёт Разум) ----
    ((thread_t*)rt)->state = THREAD_ZOMBIE;               // прежние потоки теста уходят
    a->state = THREAD_ZOMBIE; b->state = THREAD_ZOMBIE;
    sched_yield();
    for (int i = 0; i < 3; i++) tick();
    sched_set_quantum(4);
    thread_t* wa = sched_create("wa", dummy, 0, PRIO_NORMAL);
    thread_t* wb = sched_create("wb", dummy, 0, PRIO_NORMAL);
    CHECK(wa && wb);
    wa->pid = 501; wb->pid = 502;
    CHECK(sched_runnable() >= 2);
    CHECK(sched_set_proc_weight(501, 8) == 1 && wa->weight == 8 && sched_set_proc_weight(999, 8) == 0);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    for (int i = 0; i < 4000; i++) tick();
    double r = (double)cpu_ticks[wa->id] / (double)(cpu_ticks[wb->id] ? cpu_ticks[wb->id] : 1);
    CHECK(r > 1.7 && r < 2.3);                              // вес 8 против 4: вдвое больше процессора, но не всё
    CHECK(cpu_ticks[wb->id] > 800);                         // голодания нет
    sched_set_proc_weight(501, 4);
    CHECK(sched_set_proc_limit(502, 25) == 1 && wb->limit == 25);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    int slept = 0;
    for (int i = 0; i < 4000; i++) { tick(); if (wb->state == THREAD_SLEEPING) slept = 1; }
    CHECK(slept);                                           // исчерпав долю, поток спит до конца окна
    CHECK(cpu_ticks[wb->id] <= 4000 * 27 / 100 && cpu_ticks[wb->id] >= 4000 * 20 / 100);
    CHECK(cpu_ticks[wa->id] >= 4000 * 60 / 100);            // освободившееся время досталось другому
    sched_set_proc_limit(502, 100);
    CHECK(wb->limit == 0);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    for (int i = 0; i < 4000; i++) tick();
    r = (double)cpu_ticks[wa->id] / (double)(cpu_ticks[wb->id] ? cpu_ticks[wb->id] : 1);
    CHECK(r > 0.8 && r < 1.25);                             // снято - снова поровну
    // один поток с ограничением 50%: половину окна ядро свободно (холостой поток)
    wa->state = THREAD_ZOMBIE;
    if (sched_current() == wa) sched_yield();
    sched_set_proc_limit(502, 50);
    memset(cpu_ticks, 0, sizeof cpu_ticks);
    for (int i = 0; i < 2000; i++) tick();
    CHECK(cpu_ticks[wb->id] >= 2000 * 45 / 100 && cpu_ticks[wb->id] <= 2000 * 55 / 100);
    sched_set_proc_limit(502, 100);

    // ---- 9. рефлекс: проснувшийся поток программы в фокусе (вес 6) вытесняет фоновый (вес 4) сразу, не дожидаясь
    //         конца кванта; вытесненный фон идёт следующим с остатком кванта, а не в хвост ----
    {
        sched_set_quantum(20);
        thread_t* fg = sched_create("fg", dummy, 0, PRIO_NORMAL);
        fg->pid = 503;
        sched_set_proc_weight(503, 6);
        thread_t* bg2 = sched_create("bg2", dummy, 0, PRIO_NORMAL);
        bg2->pid = 504;
        for (int i = 0; i < 200 && sched_current() != fg; i++) tick();
        CHECK(sched_current() == fg);
        sched_sleep_ticks(5);                                    // фокус ждёт ввода
        for (int i = 0; i < 200 && !(sched_current() != fg && sched_current()->quantum_left > 10); i++) tick();
        thread_t* running_bg = (thread_t*)sched_current();
        CHECK(running_bg != fg && running_bg->weight == 4);
        fg->wake_tick = fake_ticks + 1;
        sched_set_wake_tick(fg, fake_ticks + 1);
        tick();                                                  // срок: фокус проснулся
        CHECK(sched_current() == fg);                            // и пошёл сразу, хотя у фона квант не кончился
        CHECK(running_bg->state == THREAD_READY && running_bg->quantum_left > 0);
        sched_sleep_ticks(1000);                                 // фокус снова ждёт: идёт вытесненный фон, с остатком
        CHECK(sched_current() == running_bg);
        fg->state = THREAD_ZOMBIE; bg2->state = THREAD_ZOMBIE;
        if (sched_current() == bg2) sched_yield();
        sched_set_quantum(4);
    }

    // ---- 10. рычаг Разума: ядро отдано окну в фокусе - потоки других программ с него уходят, свои закрепления
    //          программ и потоки ядра не трогаются; освобождение возвращает всё ----
    {
        extern unsigned host_cpu;
        for (unsigned c = 2; c <= 3; c++) { percpu_alloc(c); host_cpu = c; CHECK(sched_attach_cpu() == 0); }
        host_cpu = 0;
        thread_t* f1 = sched_create("focus", dummy, 0, PRIO_NORMAL); f1->pid = 600;
        thread_t* b1 = sched_create("bg", dummy, 0, PRIO_NORMAL);    b1->pid = 601;
        thread_t* pin = sched_create_on("pinned", dummy, 0, PRIO_NORMAL, cm_from64(8)); pin->pid = 601;
        thread_t* kt = sched_create("kthread", dummy, 0, PRIO_NORMAL);
        CHECK(sched_reserve_core(3, 600) >= 1 && sched_reserved_core() == 3);
        CHECK(!cm_test(&b1->affinity, 3) && cm_test(&b1->affinity, 2) && (b1->flags & THREAD_F_OFFCORE));
        CHECK(cm_test(&f1->affinity, 3) && !(f1->flags & THREAD_F_OFFCORE));
        CHECK(cm_is64(&pin->affinity, 8));                        // закрепление программы - её решение
        CHECK(cm_is_all(&kt->affinity));                          // потоки ядра не трогаются
        CHECK(b1->rq_cpu != 3);                                   // из очереди отданного ядра ушёл
        thread_t* b2 = sched_create("bg-new", dummy, 0, PRIO_NORMAL); b2->pid = 601;
        sched_reserve_core(3, 600);                               // повтор подхватывает новый поток
        CHECK(!cm_test(&b2->affinity, 3));
        sched_reserve_core(2, 601);                               // фокус сменился: теперь 601, ядро 2
        CHECK(cm_is_all(&b1->affinity) && !cm_test(&f1->affinity, 2) && cm_test(&f1->affinity, 3));
        sched_reserve_core(-1, 0);
        CHECK(cm_is_all(&b1->affinity) && cm_is_all(&b2->affinity) && cm_is_all(&f1->affinity) && sched_reserved_core() == -1);
        f1->state = b1->state = pin->state = kt->state = b2->state = THREAD_ZOMBIE;
    }

    printf("%s\n", fails ? "FAILED" : "ALL SCHEDULER TESTS PASSED");
    return fails != 0;
}
