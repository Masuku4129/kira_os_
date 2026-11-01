/* Смешивание звука: сумма источников с ограничением 16 бит. */
#include <stdio.h>
#include <stdint.h>
#include "../audio.h"
int main(void) {
    int16_t a[4] = { 1000, 30000, -30000, 0 }, b[4] = { 2000, 10000, -10000, -5 }, out[4];
    const int16_t* in[2] = { a, b };
    audio_mix(out, in, 2, 4);
    if (out[0] != 3000 || out[1] != 32767 || out[2] != -32768 || out[3] != -5) { printf("FAIL mix\n"); return 1; }
    audio_mix(out, in, 0, 4);
    if (out[0] || out[1] || out[2] || out[3]) { printf("FAIL silence\n"); return 1; }
    printf("audio: mixing with 16-bit clipping, silence passed\n");
    return 0;
}
