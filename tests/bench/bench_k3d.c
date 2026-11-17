// Замер нового движка k3d на хосте (gcc -O2 -msse2) на кадрах программы cube3d (userland/cube3d.c с заглушкой системных вызовов):
//  1) "как раньше": куб и октаэдр, плоские цвета, свет по вершинам, без текстур - сравнимо со старым рендерером (bench_legacy.c);
//  2) полная новая сцена: кирпичный куб (свет по пикселям, билинейно, мипы), планета, октаэдр, стеклянный куб (прозрачность).
// Для каждого числа потоков-помощников (0, 1, 2, 3): кадров/с, нс на кадр, нс на записанный пиксель, нс на треугольник, время фаз.
// Хост - не Kira: это сравнение алгоритмов, а не цифры системы (в QEMU - сценарий cube3d-bench).
#include <time.h>
#include "stub/kira.h"
#include "stub/kira_threads.h"
char stub_log[4096];
#include "../../userland/cube3d.c"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

// сцена "как раньше": два тела, плоские грани (цвет граней куба как у прежнего рендерера), без текстур
static int render_legacy_like(surf_t* t, u64 t_ns) {
    k3d_cmdbuf_t cb; k3d_cb_init(&cb, words, sizeof words / 4);
    k3d_mat4 proj = k3d_mat4_perspective(1.7320508f, (float)t->w / (float)t->h, 0.5f, 30.0f);
    v3 l = {-0.45f, 0.70f, -0.55f}; float ll = __builtin_sqrtf(v_dot(l, l));
    float ldir[3] = { l.x / ll, l.y / ll, l.z / ll }, lcol[3] = { 0.78f, 0.78f, 0.78f }, lamb[3] = { 0.22f, 0.22f, 0.22f };
    k3d_cb_clear(&cb, 0x000A0E22, 0x001F2A55, 1.0f); k3d_cb_matrix(&cb, K3D_SLOT_PROJ, &proj); k3d_cb_light(&cb, ldir, lcol, lamb);
    const k3d_u32 F = K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_DEPTH_WRITE | K3D_LIGHT_VERTEX;
    float ay = angle_ns(t_ns, 14000000000ULL), ax = angle_ns(t_ns, 23000000000ULL);
    k3d_mat4 ry = k3d_mat4_rot_y(ay), rx = k3d_mat4_rot_x(ax), rot = k3d_mat4_mul(&ry, &rx), s = k3d_mat4_scale(1.25f, 1.25f, 1.25f), p = k3d_mat4_translate(0, 0, 6.2f);
    k3d_mat4 m = k3d_mat4_mul(&rot, &s); m = k3d_mat4_mul(&p, &m);
    draw_model(&cb, 0, &m, F, 0xFFFFFFFF, -1);
    float ao = angle_ns(t_ns, 6000000000ULL);
    k3d_mat4 ory = k3d_mat4_rot_y(-ao * 3.0f), orx = k3d_mat4_rot_x(ao * 2.0f), orot = k3d_mat4_mul(&ory, &orx), os = k3d_mat4_scale(0.75f, 0.75f, 0.75f);
    k3d_mat4 op = k3d_mat4_translate(3.0f * k3d_cosf(ao), 1.1f * k3d_sinf(ao * 2.0f), 6.2f + 2.6f * k3d_sinf(ao));
    m = k3d_mat4_mul(&orot, &os); m = k3d_mat4_mul(&op, &m);
    draw_model(&cb, 1, &m, F, 0xFFFFFFFF, -1);
    int rc = k3d_submit(sc.c, words, cb.n, (k3d_u32*)t->px, t->pitch);
    sc.last = *k3d_stats(sc.c);
    return rc;
}

typedef int (*frame_fn)(surf_t*, u64);
static void measure(const char* name, frame_fn fn, int workers, int frames) {
    static u32 px[360 * 270]; surf_t s = { px, 360, 270, 360 };
    u64 ab = 0; { k3d_config_t cfg; k3d_config_default(&cfg, 360, 270, workers); k3d_kira_sys(&cfg.sys); ab = (k3d_mem_size(&cfg) + 4095) & ~4095ull; }
    void* arena = aligned_alloc(4096, ab);
    if (scene_init(arena, ab, 360, 270, workers) != 0) { printf("init failed\n"); return; }
    for (int i = 0; i < 50; i++) fn(&s, 1000000000ULL * i / 7);
    double t0 = now(); unsigned long long pxs = 0, tris = 0, geo = 0, ras = 0;
    for (int i = 0; i < frames; i++) { fn(&s, 1000000000ULL * i / 97); pxs += sc.last.px_written; tris += sc.last.tris_drawn; geo += sc.last.ns_geometry; ras += sc.last.ns_raster; }
    double dt = now() - t0;
    printf("%-22s helpers=%d: %8.1f fps, %7.0f ns/frame (geometry %6.0f + tiles %6.0f), %6.2f ns/px written (%.0f px/frame), %6.0f ns/triangle (%.1f tris/frame), mem %llu KB\n",
           name, workers, frames / (dt / 1e9), dt / frames, (double)geo / frames, (double)ras / frames, dt / pxs, (double)pxs / frames, dt / tris, (double)tris / frames, (unsigned long long)(ab / 1024));
    scene_close(); free(arena);
}

int main(int argc, char** argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 2000;
    prepare_models();
    for (int w = 0; w <= 3; w++) measure("k3d, scene as before", render_legacy_like, w, frames);
    for (int w = 0; w <= 3; w++) measure("k3d, new full scene", render_scene, w, frames);
    return 0;
}
