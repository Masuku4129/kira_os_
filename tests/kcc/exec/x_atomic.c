#include "ktest.h"
/* __atomic_* и __sync_* над целыми 1/2/4/8 байт и указателями; потоков нет, проверяется результат каждой операции */
static int flag;
static void *gp;
int main(void) {
    unsigned char b = 250; unsigned short h = 65530; int i = -5; unsigned u = 5; long l = 1L << 40; unsigned long ul = 7;
    pn(__atomic_fetch_add(&b, 10, __ATOMIC_SEQ_CST)); pn(b);
    pn(__atomic_fetch_sub(&h, 10, __ATOMIC_RELAXED)); pn(h);
    pn(__atomic_add_fetch(&i, 100, __ATOMIC_ACQ_REL)); pn(i);
    pn(__atomic_sub_fetch(&u, 6, __ATOMIC_SEQ_CST)); pn(u);
    pn(__atomic_fetch_and(&l, 0xff00ff00ffL << 8, __ATOMIC_SEQ_CST)); pn(l);
    pn(__atomic_fetch_or(&ul, 0xf0, __ATOMIC_SEQ_CST)); pn(ul);
    pn(__atomic_fetch_xor(&ul, 0xff, __ATOMIC_SEQ_CST)); pn(ul);
    pn(__atomic_xor_fetch(&b, 0x0f, __ATOMIC_SEQ_CST)); pn(b);
    pn(__atomic_or_fetch(&h, 0x1000, __ATOMIC_SEQ_CST)); pn(h);
    pn(__atomic_and_fetch(&i, 0x7f, __ATOMIC_SEQ_CST)); pn(i);
    pn(__atomic_fetch_nand(&u, 3, __ATOMIC_SEQ_CST)); pn(u);
    pn(__atomic_nand_fetch(&b, 0x3c, __ATOMIC_SEQ_CST)); pn(b);
    pn(__atomic_exchange_n(&i, 77, __ATOMIC_ACQUIRE)); pn(i);
    pn(__atomic_exchange_n(&b, 200, __ATOMIC_SEQ_CST)); pn(b);
    __atomic_store_n(&h, 1234, __ATOMIC_RELEASE); pn(__atomic_load_n(&h, __ATOMIC_ACQUIRE));
    __atomic_store_n(&l, -9, __ATOMIC_SEQ_CST); pn(__atomic_load_n(&l, __ATOMIC_SEQ_CST));
    signed char sc = -3; pn(__atomic_load_n(&sc, __ATOMIC_SEQ_CST)); pn(__atomic_fetch_sub(&sc, 130, __ATOMIC_SEQ_CST)); pn(sc);
    short ss = -300; pn(__atomic_add_fetch(&ss, -40000, __ATOMIC_SEQ_CST));
    /* compare_exchange: успех и неудача (ожидаемое обновляется) */
    int e = 77, ok = __atomic_compare_exchange_n(&i, &e, 5, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    pn(ok); pn(i); pn(e);
    e = 1; ok = __atomic_compare_exchange_n(&i, &e, 6, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
    pn(ok); pn(i); pn(e);
    unsigned char be = 200; pn(__atomic_compare_exchange_n(&b, &be, 3, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); pn(b);
    long le = -9, ld = 11; pn(__atomic_compare_exchange(&l, &le, &ld, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); pn(l);
    /* общие формы */
    long lr; __atomic_load(&l, &lr, __ATOMIC_SEQ_CST); pn(lr);
    long ns = 42; __atomic_store(&l, &ns, __ATOMIC_SEQ_CST); pn(l);
    long nx = 5, old; __atomic_exchange(&l, &nx, &old, __ATOMIC_SEQ_CST); pn(old); pn(l);
    /* указатели */
    int arr[4] = { 1, 2, 3, 4 };
    gp = &arr[0];
    void *q = __atomic_exchange_n(&gp, (void *)&arr[2], __ATOMIC_SEQ_CST); pn((int *)q == &arr[0]); pn(*(int *)gp);
    int *ip = &arr[1]; int *ipe = &arr[1];
    pn(__atomic_compare_exchange_n(&ip, &ipe, &arr[3], 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); pn(*ip);
    __atomic_fetch_add(&ip, -(long)sizeof(int), __ATOMIC_SEQ_CST); pn(*ip);
    /* флаг */
    pn(__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)); pn(__atomic_test_and_set(&flag, __ATOMIC_ACQUIRE)); __atomic_clear(&flag, __ATOMIC_RELEASE); pn(flag & 255);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); __atomic_signal_fence(__ATOMIC_ACQUIRE);
    pn(__atomic_always_lock_free(8, 0)); pn(__atomic_always_lock_free(3, 0));
    /* __sync */
    int sv = 10;
    pn(__sync_fetch_and_add(&sv, 5)); pn(__sync_add_and_fetch(&sv, 5)); pn(__sync_fetch_and_sub(&sv, 3)); pn(__sync_sub_and_fetch(&sv, 2));
    pn(__sync_fetch_and_or(&sv, 0x100)); pn(__sync_and_and_fetch(&sv, 0xf0)); pn(__sync_fetch_and_xor(&sv, 0xff)); pn(__sync_xor_and_fetch(&sv, 1));
    pn(__sync_fetch_and_nand(&sv, 0xf)); pn(sv);
    pn(__sync_bool_compare_and_swap(&sv, sv, 9)); pn(__sync_bool_compare_and_swap(&sv, 1, 2)); pn(sv);
    pn(__sync_val_compare_and_swap(&sv, 9, 33)); pn(__sync_val_compare_and_swap(&sv, 9, 44)); pn(sv);
    pn(__sync_lock_test_and_set(&sv, 8)); __sync_lock_release(&sv); pn(sv);
    __sync_synchronize();
    unsigned long long big = 0xfffffffffffffff0ULL; pn(__sync_add_and_fetch(&big, 0x20) == 0x10);
    /* цикл CAS */
    unsigned cnt = 0;
    for (int k = 0; k < 100; k++) {
        unsigned c = __atomic_load_n(&cnt, __ATOMIC_SEQ_CST);
        while (!__atomic_compare_exchange_n(&cnt, &c, c + (unsigned)k, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
    }
    pn(cnt);
    return 0;
}
