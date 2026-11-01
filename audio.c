/* Микшер звука (см. audio.h). Кольцо карты - hda_ring_*; источники - очереди программ (1 с) и тон. */
#include "audio.h"

void audio_mix(int16_t* out, const int16_t* const* in, unsigned nin, uint32_t samples) {
    for (uint32_t i = 0; i < samples; i++) {
        int32_t s = 0;
        for (unsigned k = 0; k < nin; k++) s += in[k][i];
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

#ifndef KIRA_HOST_TEST
#include "hda.h"
#include "sched.h"
#include "timer.h"
#include "spinlock.h"
#include "uart.h"
#include "proc.h"
#include "uaccess.h"
#include "heap.h"

#define SRC_FRAMES AUDIO_RATE                  /* очередь источника - 1 с */
#define LEAD (AUDIO_RATE / 10)                 /* опережение записи - 100 мс */
#define CHUNK 480                              /* смешивается по 10 мс */

typedef struct {
    int used; uint32_t pid;
    int16_t* q; uint32_t head, tail;           /* кадры: занято tail - head */
} src_t;
static src_t src[AUDIO_SRC_MAX];
static spinlock_t lk = SPINLOCK_INIT;
static volatile unsigned tone_step, tone_left; static unsigned tone_phase;
static uint64_t hw, wr, mixed_prog, underruns;
static uint32_t last_pos;
static int running;

static int16_t sine(uint32_t ph) {                                  /* та же таблица, что у hda.c */
    static const uint16_t q[17] = { 0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767 };
    uint32_t i = (ph >> 12) & 63;
    int s = i < 16 ? q[i] : i < 32 ? q[32 - i] : i < 48 ? -q[i - 32] : -q[64 - i];
    return (int16_t)(s / 4);
}

static void mix_chunk(int16_t* out, uint32_t frames) {
    static int16_t tmp[AUDIO_SRC_MAX + 1][CHUNK * 2];
    const int16_t* in[AUDIO_SRC_MAX + 1]; unsigned n = 0;
    uint64_t f = spin_lock_irqsave(&lk);
    for (int k = 0; k < AUDIO_SRC_MAX; k++) {
        src_t* s = &src[k];
        if (!s->used || s->tail == s->head) continue;
        uint32_t have = s->tail - s->head, take = have < frames ? have : frames;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t j = (s->head + i) % SRC_FRAMES;
            tmp[n][2 * i] = i < take ? s->q[2 * j] : 0; tmp[n][2 * i + 1] = i < take ? s->q[2 * j + 1] : 0;
        }
        s->head += take; mixed_prog += take;
        in[n] = tmp[n]; n++;
    }
    if (tone_left) {
        uint32_t take = tone_left < frames ? tone_left : frames;
        for (uint32_t i = 0; i < frames; i++) { int16_t v = i < take ? sine(tone_phase) : 0; tmp[n][2 * i] = tmp[n][2 * i + 1] = v; if (i < take) tone_phase += tone_step; }
        tone_left -= take;
        in[n] = tmp[n]; n++;
    }
    spin_unlock_irqrestore(&lk, f);
    audio_mix(out, in, n, frames * 2);
}

static void audio_main(void* arg) {
    (void)arg;
    uint32_t ring = hda_ring_bytes() / 4;
    int16_t* buf = hda_ring_buf();
    last_pos = hda_ring_pos() / 4; wr = hw = 0;
    for (;;) {
        uint32_t pos = hda_ring_pos() / 4;
        hw += (pos + ring - last_pos) % ring; last_pos = pos;
        if (wr < hw) { underruns++; wr = hw; }                     /* не успели: догоняем карту */
        while (wr < hw + LEAD) {
            uint32_t at = (uint32_t)(wr % ring), n = CHUNK;
            if (at + n > ring) n = ring - at;
            mix_chunk(buf + 2 * at, n);
            wr += n;
        }
        cpu_mb();
        sched_sleep_ticks(TIMER_HZ / 100 ? TIMER_HZ / 100 : 1);    /* ~10 мс */
    }
}

int audio_start(void) {
    if (running) return 0;
    for (int k = 0; k < AUDIO_SRC_MAX; k++) if (!src[k].q) src[k].q = (int16_t*)kmalloc(SRC_FRAMES * 4);
    if (hda_ring_start() != 0) return -1;
    if (!sched_create("audio", audio_main, 0, PRIO_CRITICAL)) return -2;
    running = 1;
    uart_print("[KIRA AUDIO] Mixer online: up to 4 programs + tone, 48 kHz 16-bit stereo, 100 ms ahead of the card.\n");
    return 0;
}

uint64_t audio_hw_frames(void) { return hw; }

int audio_tone(unsigned hz, unsigned ms) {
    if (!running) return -1;
    uint64_t f = spin_lock_irqsave(&lk);
    tone_phase = 0; tone_step = (uint32_t)(((uint64_t)hz << 18) / AUDIO_RATE); tone_left = AUDIO_RATE / 1000 * ms;
    spin_unlock_irqrestore(&lk, f);
    return 0;
}

void audio_print(void) {
    unsigned n = 0; for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used) n++;
    uart_print("микшер: "); uart_print(running ? "работает" : "не запущен");
    uart_print(", программ со звуком "); uart_print_dec(n); uart_print(", карта забрала кадров "); uart_print_dec(hw);
    uart_print(", от программ смешано "); uart_print_dec(mixed_prog); uart_print(", недоборов "); uart_print_dec(underruns); uart_print("\n");
}

int64_t audio_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint32_t pid = proc_current_pid();
    if (!running) return -2;
    if (op == 0) {                                                  /* open */
        uint64_t f = spin_lock_irqsave(&lk);
        int h = -9;
        for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used && src[k].pid == pid) { h = -3; break; }
        if (h == -9) for (int k = 0; k < AUDIO_SRC_MAX; k++) if (!src[k].used && src[k].q) { src[k].used = 1; src[k].pid = pid; src[k].head = src[k].tail = 0; h = k; break; }
        spin_unlock_irqrestore(&lk, f);
        return h;
    }
    if (a1 >= AUDIO_SRC_MAX || !src[a1].used || src[a1].pid != pid) return -8;
    src_t* s = &src[a1];
    if (op == 2) { uint64_t f = spin_lock_irqsave(&lk); s->used = 0; spin_unlock_irqrestore(&lk, f); return 0; }
    if (op == 1) {                                                  /* write: сколько влезло (кадрами по 4 байта) */
        uint64_t frames = a3 / 4, done = 0;
        if (a3 > (1u << 22) || (a3 && !user_range(a2, a3, 0))) return -10;
        int16_t tmp[512 * 2];                                        /* своя копия на стеке вызова: программы пишут параллельно */
        while (done < frames) {
            uint32_t space = SRC_FRAMES - (s->tail - s->head);
            if (!space) break;
            uint64_t n = frames - done; if (n > space) n = space; if (n > 512) n = 512;
            if (copy_from_user(tmp, a2 + done * 4, n * 4)) return done ? (int64_t)(done * 4) : -10;
            uint64_t f = spin_lock_irqsave(&lk);
            for (uint64_t i = 0; i < n; i++) { uint32_t j = (s->tail + (uint32_t)i) % SRC_FRAMES; s->q[2 * j] = tmp[2 * i]; s->q[2 * j + 1] = tmp[2 * i + 1]; }
            s->tail += (uint32_t)n;
            spin_unlock_irqrestore(&lk, f);
            done += n;
        }
        return (int64_t)(done * 4);
    }
    return -10;
}

void audio_proc_gone(uint32_t pid) {
    uint64_t f = spin_lock_irqsave(&lk);
    for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used && src[k].pid == pid) src[k].used = 0;
    spin_unlock_irqrestore(&lk, f);
}
#endif
