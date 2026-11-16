// Замер старого рендерера cube3d на хосте (gcc -O2 -msse2): кадров/с, нс на кадр, нс на показанный пиксель, нс на треугольник.
// Старый код лежит в cube3d_legacy.c (копия userland/cube3d.c до перехода на движок k3d); в систему не входит.
#define _GNU_SOURCE
#include <time.h>
#include "stub/kira.h"
char stub_log[4096];
#include "cube3d_legacy.c"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
int main(void) {
    static u32 px[360 * 270]; surf_t s = { px, 360, 270, 360 };
    u32* z = calloc(360 * 270, 4); scene_init(z, 360, 270);
    for (int i = 0; i < 50; i++) render_scene(&s, 1000000000ULL * i / 7);
    int N = 2000; double t0 = now(); unsigned long long pxs = 0, tris = 0;
    for (int i = 0; i < N; i++) { render_scene(&s, 1000000000ULL * i / 97); pxs += stats.pixels_written; tris += stats.tris_drawn; }
    double dt = now() - t0;
    printf("legacy cube3d 360x270: %.1f fps, %.0f ns/frame, %.2f ns/shaded px (%.0f px/frame), %.0f ns/triangle (%.1f tris/frame)\n",
           N / (dt / 1e9), dt / N, dt / pxs, (double)pxs / N, dt / tris, (double)tris / N);
    return 0;
}
