/* melody [частота] [mono] - гамма до мажор (8 нот по 200 мс) через звук программ (k_snd_*): синус по таблице.
   По умолчанию 48 кГц стерео; с аргументами - другой формат (ядро пересчитает в 48 кГц). Звук смешивается с
   другими программами и тоном beep в ядре. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }
static short sine(u32 ph) {
    static const unsigned short q[17] = { 0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767 };
    u32 i = (ph >> 12) & 63;
    int s = i < 16 ? q[i] : i < 32 ? q[32 - i] : i < 48 ? -q[i - 32] : -q[64 - i];
    return (short)(s / 5);
}
static short buf[1920 * 2];                                   /* 20 мс при частоте до 96 кГц */

int umain(void) {
    char a[32]; u32 rate = 48000, ch = 2;
    if (k_args(a, sizeof a) > 0) {
        const char* p = a; u32 v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0');
        if (v) rate = v;
        while (*p == ' ') p++;
        if (p[0] == 'm') ch = 1;
    }
    i64 h = k_snd_open_fmt(rate, ch);
    if (h < 0) { say("melody: no sound output (or format not supported)\n"); return 1; }
    u32 per = rate / 50;                                      /* кадров в 20 мс */
    static const u32 hz[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
    u64 frames = 0;
    for (int n = 0; n < 8; n++) {
        u32 step = (u32)(((u64)hz[n] << 18) / rate), ph = 0;
        for (int c = 0; c < 10; c++) {                         /* 10 кусков по 20 мс = 200 мс */
            for (u32 i = 0; i < per; i++) {
                short v = (i > per - per / 16 && c == 9) ? 0 : sine(ph);
                if (ch == 2) { buf[2 * i] = buf[2 * i + 1] = v; } else buf[i] = v;
                ph += step;
            }
            u64 off = 0, bytes = (u64)per * 2 * ch;
            while (off < bytes) {                              /* очередь полна - подождать */
                i64 r = k_snd_write(h, (const char*)buf + off, bytes - off);
                if (r < 0) { say("melody: write failed\n"); return 1; }
                off += (u64)r;
                if (off < bytes) k_sleep(5);
            }
            frames += per;
        }
    }
    k_sleep(150);                                             /* дать доиграть очереди (1 с) */
    k_snd_close(h);
    say("melody: 8 notes, "); say_u(frames); say(" frames queued");
    if (rate != 48000 || ch != 2) { say(" at "); say_u(rate); say(ch == 1 ? " Hz mono" : " Hz stereo"); }
    say("\n");
    return 0;
}
