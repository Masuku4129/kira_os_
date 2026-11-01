/* melody - гамма до мажор (8 нот по 200 мс) через звук программ (k_snd_*): синус по таблице, 48 кГц, стерео.
   Звук смешивается с другими программами и тоном beep в ядре. */
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
static short buf[960 * 2];                                    /* 20 мс */

int umain(void) {
    i64 h = k_snd_open();
    if (h < 0) { say("melody: no sound output\n"); return 1; }
    static const u32 hz[8] = { 262, 294, 330, 349, 392, 440, 494, 523 };
    u64 frames = 0;
    for (int n = 0; n < 8; n++) {
        u32 step = (u32)(((u64)hz[n] << 18) / 48000), ph = 0;
        for (int c = 0; c < 10; c++) {                         /* 10 кусков по 20 мс = 200 мс */
            for (int i = 0; i < 960; i++) { short v = (i > 900 && c == 9) ? 0 : sine(ph); buf[2 * i] = buf[2 * i + 1] = v; ph += step; }
            u64 off = 0;
            while (off < sizeof buf) {                         /* очередь полна - подождать */
                i64 r = k_snd_write(h, (const char*)buf + off, sizeof buf - off);
                if (r < 0) { say("melody: write failed\n"); return 1; }
                off += (u64)r;
                if (off < sizeof buf) k_sleep(5);
            }
            frames += 960;
        }
    }
    k_sleep(150);                                             /* дать доиграть очереди (1 с) */
    k_snd_close(h);
    say("melody: 8 notes, "); say_u(frames); say(" frames queued\n");
    return 0;
}
