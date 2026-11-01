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
   между вызовами (поток режется на куски как угодно - результат тот же). Возвращает число выходных кадров.
   Вход выше 48 кГц (понижение): оконный sinc-фильтр (Кайзер, окно 40 выходных кадров), полифазно по
   дробной позиции, целые числа, без кучи: полоса до 20 кГц сохранена (в пределах 0,2 дБ), от 24 кГц (новая
   частота Найквиста) подавление не хуже 50 дБ (проверка - tests/audio_test.c); постоянная составляющая точно 1:1, насыщение до 16 бит. Звук выходит с задержкой
   AUDIO_FIR_HALF кадров (центрированное ядро; без прибавки фазы) - места в out должно быть не меньше in_frames + 1.
   Вход не выше 48 кГц (повышение) - линейная интерполяция. */
#define AUDIO_FIR_HIST 128                     /* история входа: степень двойки, >= 2*AUDIO_FIR_HALF*2 + 2 кадров (вход <= 96 кГц) */
typedef struct {
    uint32_t step, pos; int16_t prev[2]; int have; unsigned ch;   /* step - входная частота */
    int fir; uint64_t nin, m;                  /* фильтр: принято входных кадров, номер следующего выходного */
    int16_t hist[AUDIO_FIR_HIST * 2];          /* последние входные кадры (L, R) по кругу */
} audio_rs_t;
void     audio_rs_init(audio_rs_t* r, unsigned in_rate, unsigned ch);
uint32_t audio_resample(audio_rs_t* r, const int16_t* in, uint32_t in_frames, int16_t* out, uint32_t max_out);

/* Звуковая карта для микшера: вечное кольцо вывода (48 кГц 16 бит стерео), позиция карты в нём; service - что
   карте нужно каждые ~10 мс (AC'97: сдвинуть последний буфер, чтобы не встала); вход - такое же кольцо записи. */
typedef struct {
    const char* name;
    int (*start)(void);
    uint32_t (*pos)(void);                     /* байт от начала кольца вывода */
    int16_t* buf; uint32_t bytes;
    void (*service)(void);                     /* 0 - не нужно */
    int in_ready; uint32_t (*in_pos)(void); const int16_t* in_buf; uint32_t in_bytes; const char* in_name;
    void (*print)(void);                       /* подробности карты (команда sound) */
} audio_card_t;
int  audio_attach(const audio_card_t* c);      /* карта готова: запустить поток смешивания; 0 - готово */
int  audio_running(void);
int  audio_tone(unsigned hz, unsigned ms);     /* тон (beep) как источник; 0 - принят */
void audio_set_lead_ms(uint32_t ms);           /* опережение записи перед картой, мс (по умолчанию 100) */
void audio_set_period_ms(uint32_t ms);         /* период потока смешивания, мс (по умолчанию 10) */
void audio_stats(uint64_t* mixed_frames, uint64_t* under);   /* смешано кадров от программ и недоборов с запуска */
uint64_t audio_hw_frames(void);                /* сколько кадров карта забрала с запуска */
void audio_print(void);                        /* состояние микшера (команда sound) */
void audiocmd_sound(const char* arg);
void audiocmd_beep(const char* arg);

/* Для программ (системные вызовы): поток звука принадлежит процессу. */
/* 0 open(частота или 0 = 48000, каналы 1/2 или 0 = 2), 1 write(h, buf, байт) -> принято байт, 2 close(h),
   3 volume(h, 0..100) - громкость своего потока. */
int64_t audio_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3);
int  audio_master(int percent);
/* Запись со входа карты (микрофон/линейный): frames кадров 48 кГц стерео, начиная с «сейчас». 0 - записано;
   -1 - входа нет; -3 - запись уже идёт (один писатель); -5 - срок вышел (карта не пишет). */
int  audio_capture(int16_t* out, uint32_t frames, uint64_t timeout_ticks);
int64_t audio_record_sys(uint64_t ubuf, uint64_t bytes, uint64_t timeout_ms);   /* вызов 43 */
void audiocmd_record(const char* arg);           /* команда record [мс] */                /* общая громкость: <0 - только узнать; возврат - текущая */
void audio_proc_gone(uint32_t pid);
#endif
