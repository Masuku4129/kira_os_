#ifndef KIRA_AUDIO_H
#define KIRA_AUDIO_H
#include <stdint.h>
/* Звук для системы и программ: поток «audio» смешивает источники (программы, тон beep) и пишет в кольцо звуковой
   карты с опережением ~100 мс. Карта - 48 кГц, 16 бит, стерео. Программа открывает поток со своей частотой
   (8000..96000) и числом каналов (1 или 2): пересчёт частоты - линейной интерполяцией при записи. Громкость - у
   каждого потока и общая (0..100), применяется при смешивании. */
#define AUDIO_RATE 48000
#define AUDIO_SRC_MAX 4

/* Чистые функции (тест на хосте).
   audio_mix: out[i] = (сумма in[k][i] * gain[k]) * master с ограничением 16 бит; усиления - доли 65536 (gain 0 - все 65536). */
void audio_mix(int16_t* out, const int16_t* const* in, const uint32_t* gain, unsigned nin, uint32_t samples, uint32_t master);
uint32_t audio_vol_gain(unsigned percent);     /* 0..100 -> доля 65536 (по громкости слуха: квадрат) */
/* Пересчёт частоты: вход in_rate, ch каналов -> 48 кГц стерео. Состояние переносит дробную позицию и прошлый кадр
   между вызовами (поток режется на куски как угодно - результат тот же). Возвращает число выходных кадров. */
typedef struct { uint32_t step, pos; int16_t prev[2]; int have; unsigned ch; } audio_rs_t;   /* step - входная частота */
void     audio_rs_init(audio_rs_t* r, unsigned in_rate, unsigned ch);
uint32_t audio_resample(audio_rs_t* r, const int16_t* in, uint32_t in_frames, int16_t* out, uint32_t max_out);

int  audio_start(void);                        /* карта готова: запустить поток смешивания; 0 - готово */
int  audio_tone(unsigned hz, unsigned ms);     /* тон (beep) как источник; 0 - принят */
uint64_t audio_hw_frames(void);                /* сколько кадров карта забрала с запуска */
void audio_print(void);                        /* состояние микшера (команда sound) */

/* Для программ (системные вызовы): поток звука принадлежит процессу. */
/* 0 open(частота или 0 = 48000, каналы 1/2 или 0 = 2), 1 write(h, buf, байт) -> принято байт, 2 close(h),
   3 volume(h, 0..100) - громкость своего потока. */
int64_t audio_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
int  audio_master(int percent);                /* общая громкость: <0 - только узнать; возврат - текущая */
void audio_proc_gone(uint32_t pid);
#endif
