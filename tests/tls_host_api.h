/* Хост-тест TLS: типы и вызовы программы Киры, реализованные в tests/tls_host.c поверх сокетов Linux. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef long long i64;
i64 k_net_send(i64 h, const void* p, u64 n);
i64 k_net_recv(i64 h, void* p, u32 n, u32 timeout_ms);
i64 k_random(void* buf, u64 n);
