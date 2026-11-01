/* rec [мс] - запись со входа звуковой карты (микрофон / линейный вход) через ядро (k_snd_record): до 1 с,
   48 кГц 16 бит стерео; файл WAV - в песочницу (/data/rec/last.wav), в терминал - уровень (пик, RMS). */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }
static u8 wav[44 + 48000 * 4];
static void le32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void le16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

int umain(void) {
    char a[16]; u32 ms = 1000;
    if (k_args(a, sizeof a) > 0 && a[0] >= '0' && a[0] <= '9') { ms = 0; for (const char* p = a; *p >= '0' && *p <= '9'; p++) ms = ms * 10 + (u32)(*p - '0'); }
    if (ms < 10) ms = 10;
    if (ms > 1000) ms = 1000;
    u32 frames = 48 * ms, bytes = frames * 4;
    i64 r = k_snd_record(wav + 44, bytes, 3000);
    if (r == -1) { say("rec: no sound input (codec without microphone/line in)\n"); return 1; }
    if (r < 0) { say("rec: recording failed\n"); return 1; }
    const char* h = "RIFF"; for (int i = 0; i < 4; i++) wav[i] = (u8)h[i];
    le32(wav + 4, 36 + bytes);
    h = "WAVEfmt "; for (int i = 0; i < 8; i++) wav[8 + i] = (u8)h[i];
    le32(wav + 16, 16); le16(wav + 20, 1); le16(wav + 22, 2); le32(wav + 24, 48000); le32(wav + 28, 48000 * 4); le16(wav + 32, 4); le16(wav + 34, 16);
    h = "data"; for (int i = 0; i < 4; i++) wav[36 + i] = (u8)h[i];
    le32(wav + 40, bytes);
    const short* s = (const short*)(wav + 44);
    u32 peak = 0; u64 sq = 0;
    for (u32 i = 0; i < frames * 2; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if ((u32)v > peak) peak = (u32)v; sq += (u64)((i64)s[i] * s[i]); }
    u64 m = sq / (frames * 2), rms = 0; while ((rms + 1) * (rms + 1) <= m) rms++;
    say("rec: "); say_u(frames); say(" frames (48 kHz stereo), peak "); say_u(peak); say(", RMS "); say_u(rms);
    if (k_file_write("last.wav", wav, 44 + bytes) >= 0) say(", saved to /data/rec/last.wav");
    say("\n");
    return 0;
}
