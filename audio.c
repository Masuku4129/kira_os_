/* Микшер звука (см. audio.h). Кольцо карты - hda_ring_*; источники - очереди программ (1 с) и тон. */
#include "audio.h"

void audio_mix(int16_t* out, const int16_t* const* in, const uint32_t* gain, unsigned nin, uint32_t samples, uint32_t master) {
    for (uint32_t i = 0; i < samples; i++) {
        int64_t s = 0;
        for (unsigned k = 0; k < nin; k++) s += (int64_t)in[k][i] * (gain ? gain[k] : 65536u);
        s = (s >> 16) * master >> 16;
        out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}

uint32_t audio_vol_gain(unsigned p) { if (p > 100) p = 100; return p * p * 65536u / 10000u; }

void audio_rs_init(audio_rs_t* r, unsigned in_rate, unsigned ch) {
    r->step = in_rate; r->pos = 0;                                  /* позиция - в долях 1/48000 входного кадра: точно */ r->have = 0; r->ch = ch == 1 ? 1 : 2;
    r->prev[0] = r->prev[1] = 0;
}

uint32_t audio_resample(audio_rs_t* r, const int16_t* in, uint32_t nin, int16_t* out, uint32_t max_out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < nin; i++) {
        int16_t c0 = in[i * r->ch], c1 = r->ch == 2 ? in[i * r->ch + 1] : c0;
        if (!r->have) { r->prev[0] = c0; r->prev[1] = c1; r->have = 1; continue; }   /* первый кадр - только опора */
        while (r->pos < AUDIO_RATE && n < max_out) {                 /* точки между прошлым и этим кадром */
            int32_t w = (int32_t)r->pos;
            out[2 * n]     = (int16_t)(r->prev[0] + ((int32_t)c0 - r->prev[0]) * w / AUDIO_RATE);
            out[2 * n + 1] = (int16_t)(r->prev[1] + ((int32_t)c1 - r->prev[1]) * w / AUDIO_RATE);
            n++; r->pos += r->step;
        }
        r->pos -= AUDIO_RATE;
        r->prev[0] = c0; r->prev[1] = c1;
    }
    return n;
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
    int16_t* q; uint32_t head, tail;           /* кадры 48 кГц стерео: занято tail - head */
    audio_rs_t rs; unsigned rate, ch;          /* формат программы и пересчёт */
    uint32_t gain;                             /* громкость потока (доля 65536) */
} src_t;
static src_t src[AUDIO_SRC_MAX];
static spinlock_t lk = SPINLOCK_INIT;
static volatile unsigned tone_step, tone_left; static unsigned tone_phase;
static uint64_t hw, wr, mixed_prog, underruns;
static uint32_t last_pos;
static int running;
static uint32_t master_gain = 65536; static int master_pct = 100;

static int16_t sine(uint32_t ph) {                                  /* та же таблица, что у hda.c */
    static const uint16_t q[17] = { 0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767 };
    uint32_t i = (ph >> 12) & 63;
    int s = i < 16 ? q[i] : i < 32 ? q[32 - i] : i < 48 ? -q[i - 32] : -q[64 - i];
    return (int16_t)(s / 4);
}

static void mix_chunk(int16_t* out, uint32_t frames) {
    static int16_t tmp[AUDIO_SRC_MAX + 1][CHUNK * 2];
    const int16_t* in[AUDIO_SRC_MAX + 1]; uint32_t gain[AUDIO_SRC_MAX + 1]; unsigned n = 0;
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
        in[n] = tmp[n]; gain[n] = s->gain; n++;
    }
    if (tone_left) {
        uint32_t take = tone_left < frames ? tone_left : frames;
        for (uint32_t i = 0; i < frames; i++) { int16_t v = i < take ? sine(tone_phase) : 0; tmp[n][2 * i] = tmp[n][2 * i + 1] = v; if (i < take) tone_phase += tone_step; }
        tone_left -= take;
        in[n] = tmp[n]; gain[n] = 65536; n++;
    }
    uint32_t mg = master_gain;
    spin_unlock_irqrestore(&lk, f);
    audio_mix(out, in, gain, n, frames * 2, mg);
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
    uart_print(", от программ смешано "); uart_print_dec(mixed_prog); uart_print(", недоборов "); uart_print_dec(underruns);
    uart_print(", громкость "); uart_print_dec((uint64_t)master_pct); uart_print("%\n");
    for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used) {
        uart_print("  поток "); uart_print_dec((uint64_t)k); uart_print(": процесс "); uart_print_dec(src[k].pid);
        uart_print(", "); uart_print_dec(src[k].rate); uart_print(" Гц, каналов "); uart_print_dec(src[k].ch);
        uart_print(src[k].rate != AUDIO_RATE || src[k].ch != 2 ? " (пересчёт в 48 кГц стерео)\n" : "\n");
    }
}

int audio_master(int pct) {
    if (pct >= 0) {
        if (pct > 100) pct = 100;
        uint64_t f = spin_lock_irqsave(&lk);
        master_pct = pct; master_gain = audio_vol_gain((unsigned)pct);
        spin_unlock_irqrestore(&lk, f);
    }
    return master_pct;
}

int64_t audio_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint32_t pid = proc_current_pid();
    if (!running) return -2;
    if (op == 0) {                                                  /* open(частота, каналы) */
        unsigned rate = a1 ? (unsigned)a1 : AUDIO_RATE, ch = a2 ? (unsigned)a2 : 2;
        if (a1 > 96000 || rate < 8000 || ch > 2) return -10;
        uint64_t f = spin_lock_irqsave(&lk);
        int h = -9;
        for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used && src[k].pid == pid) { h = -3; break; }
        if (h == -9) for (int k = 0; k < AUDIO_SRC_MAX; k++) if (!src[k].used && src[k].q) {
            src_t* s = &src[k];
            s->used = 1; s->pid = pid; s->head = s->tail = 0; s->rate = rate; s->ch = ch; s->gain = 65536;
            audio_rs_init(&s->rs, rate, ch);
            h = k; break;
        }
        spin_unlock_irqrestore(&lk, f);
        return h;
    }
    if (a1 >= AUDIO_SRC_MAX || !src[a1].used || src[a1].pid != pid) return -8;
    src_t* s = &src[a1];
    if (op == 2) { uint64_t f = spin_lock_irqsave(&lk); s->used = 0; spin_unlock_irqrestore(&lk, f); return 0; }
    if (op == 3) { if (a2 > 100) return -10; s->gain = audio_vol_gain((unsigned)a2); return 0; }
    if (op == 1) {                                                  /* write: сколько влезло (байт во формате программы) */
        uint32_t fb = 2 * s->ch;                                     /* байт в кадре программы */
        uint64_t frames = a3 / fb, done = 0;
        if (a3 > (1u << 22) || (a3 && !user_range(a2, a3, 0))) return -10;
        int16_t tmp[128 * 2], rs[(128 * 6 + 2) * 2];                /* до 128 кадров за раз: при 8 кГц -> до 6x */
        while (done < frames) {
            uint32_t space = SRC_FRAMES - (s->tail - s->head);
            uint64_t n = frames - done; if (n > 128) n = 128;
            uint64_t need = (n * 48000 + s->rate - 1) / s->rate + 2;  /* сколько кадров 48 кГц может выйти */
            if (space < need) break;
            if (copy_from_user(tmp, a2 + done * fb, n * fb)) return done ? (int64_t)(done * fb) : -10;
            uint64_t f = spin_lock_irqsave(&lk);
            uint32_t m = audio_resample(&s->rs, tmp, (uint32_t)n, rs, (uint32_t)(sizeof rs / 4));
            for (uint32_t i = 0; i < m; i++) { uint32_t j = (s->tail + i) % SRC_FRAMES; s->q[2 * j] = rs[2 * i]; s->q[2 * j + 1] = rs[2 * i + 1]; }
            s->tail += m;
            spin_unlock_irqrestore(&lk, f);
            done += n;
        }
        return (int64_t)(done * fb);
    }
    return -10;
}

/* ---------- запись ---------- */
static volatile int rec_busy;

int audio_capture(int16_t* out, uint32_t frames, uint64_t timeout_ticks) {
    if (!hda_in_ready()) return -1;
    uint32_t ring = hda_in_bytes() / 4;
    if (frames > ring / 2) return -10;
    if (__atomic_exchange_n(&rec_busy, 1, __ATOMIC_ACQUIRE)) return -3;
    uint32_t start = hda_in_pos() / 4;
    uint64_t until = timer_get_ticks() + timeout_ticks;
    int rc = -5;
    for (;;) {
        uint32_t avail = (hda_in_pos() / 4 + ring - start) % ring;
        if (avail >= frames) {
            cpu_mb();
            const int16_t* b = hda_in_buf();
            for (uint32_t i = 0; i < frames; i++) { uint32_t j = (start + i) % ring; out[2 * i] = b[2 * j]; out[2 * i + 1] = b[2 * j + 1]; }
            rc = 0; break;
        }
        if (timer_get_ticks() >= until) break;
        sched_sleep_ticks(1);
    }
    __atomic_store_n(&rec_busy, 0, __ATOMIC_RELEASE);
    return rc;
}

int64_t audio_record_sys(uint64_t ubuf, uint64_t bytes, uint64_t ms) {
    uint32_t frames = (uint32_t)(bytes / 4);
    if (!frames || bytes > AUDIO_RATE * 4 || !user_range(ubuf, frames * 4ull, 1)) return -10;
    int16_t* k = (int16_t*)kmalloc(frames * 4);
    if (!k) return -4;
    uint64_t t = ((ms ? ms : 2000) + (uint64_t)frames * 1000 / AUDIO_RATE) * TIMER_HZ / 1000 + 1;
    int rc = audio_capture(k, frames, t);
    int64_t r = rc ? rc : copy_to_user(ubuf, k, frames * 4ull) ? -10 : (int64_t)frames * 4;
    kfree(k);
    return r;
}

void audiocmd_record(const char* a) {
    unsigned ms = 500;
    while (a && *a == ' ') a++;
    if (a && *a >= '0' && *a <= '9') { ms = 0; while (*a >= '0' && *a <= '9') ms = ms * 10 + (unsigned)(*a++ - '0'); }
    if (ms < 10) ms = 10;
    if (ms > 1000) ms = 1000;
    uint32_t frames = AUDIO_RATE / 1000 * ms;
    int16_t* k = (int16_t*)kmalloc(frames * 4);
    if (!k) { uart_print("нет памяти\n"); return; }
    uint64_t t0 = timer_get_ticks();
    int rc = audio_capture(k, frames, (uint64_t)(ms + 1000) * TIMER_HZ / 1000);
    if (rc == -1) uart_print("входа нет (нужен кодек с микрофоном или линейным входом: в QEMU -device hda-duplex)\n");
    else if (rc == -3) uart_print("запись уже идёт (программа)\n");
    else if (rc) uart_print("карта не пишет: за срок не пришло нужного числа кадров\n");
    else {
        int peak = 0; uint64_t sq = 0;
        for (uint32_t i = 0; i < frames * 2; i++) { int v = k[i] < 0 ? -k[i] : k[i]; if (v > peak) peak = v; sq += (uint64_t)((int64_t)k[i] * k[i]); }
        uint64_t rms = 0, m = sq / (frames * 2ull);
        while ((rms + 1) * (rms + 1) <= m) rms++;
        uart_print("запись: "); uart_print(hda_in_name()); uart_print(", "); uart_print_dec(frames); uart_print(" кадров за ");
        uart_print_dec((timer_get_ticks() - t0) * 1000 / TIMER_HZ); uart_print(" мс, пик "); uart_print_dec((uint64_t)peak);
        uart_print(", RMS "); uart_print_dec(rms); uart_print(peak ? "\n" : " (тишина на входе)\n");
    }
    kfree(k);
}

void audio_proc_gone(uint32_t pid) {
    uint64_t f = spin_lock_irqsave(&lk);
    for (int k = 0; k < AUDIO_SRC_MAX; k++) if (src[k].used && src[k].pid == pid) src[k].used = 0;
    spin_unlock_irqrestore(&lk, f);
}
#endif
