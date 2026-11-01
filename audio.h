#ifndef KIRA_AUDIO_H
#define KIRA_AUDIO_H
#include <stdint.h>
/* Звук для системы и программ: поток «audio» смешивает источники (программы, тон beep) и пишет в кольцо звуковой
   карты с опережением ~100 мс. Формат источников - 48 кГц, 16 бит, стерео (кадр - 4 байта), без пересчёта частоты. */
#define AUDIO_RATE 48000
#define AUDIO_SRC_MAX 4

/* Чистая функция (тест на хосте): out[i] = сумма in[k][i] с ограничением 16 бит. */
void audio_mix(int16_t* out, const int16_t* const* in, unsigned nin, uint32_t samples);

int  audio_start(void);                        /* карта готова: запустить поток смешивания; 0 - готово */
int  audio_tone(unsigned hz, unsigned ms);     /* тон (beep) как источник; 0 - принят */
uint64_t audio_hw_frames(void);                /* сколько кадров карта забрала с запуска */
void audio_print(void);                        /* состояние микшера (команда sound) */

/* Для программ (системные вызовы): поток звука принадлежит процессу. */
int64_t audio_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);   /* 0 open, 1 write(h, buf, байт) -> принято байт, 2 close(h) */
void audio_proc_gone(uint32_t pid);
#endif
