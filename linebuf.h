#ifndef LINEBUF_H
#define LINEBUF_H

#include <stdint.h>

// Построчный буфер вывода. Каждое ядро (и каждый уровень вложенности: поток / обработчик прерывания) копит свою
// строку и отдаёт её целиком, когда встретился перевод строки: строки с разных ядер не перемешиваются
// посреди слова. Незавершённая строка (приглашение командной строки) выводится по тику, когда простояла
// LB_FLUSH_AGE тиков без продолжения.
#define LB_SIZE      512
#define LB_FLUSH_AGE 2

typedef void (*lb_emit_fn)(const char* s, unsigned n, void* ctx);

typedef struct {
    char     buf[LB_SIZE];
    unsigned len;
    unsigned age;
} linebuf_t;

void lb_putc(linebuf_t* b, char c, lb_emit_fn emit, void* ctx);   // вызывать при выключенных прерываниях ядра
void lb_tick(linebuf_t* b, lb_emit_fn emit, void* ctx);           // раз в тик: выдать залежавшийся хвост
void lb_flush(linebuf_t* b, lb_emit_fn emit, void* ctx);          // выдать немедленно

#endif
