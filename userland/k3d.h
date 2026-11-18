#ifndef K3D_H
#define K3D_H
/* k3d: программный 3D-движок для программ Ring 3 (x86-64 и ARM64). Библиотека, не программа: userland/k3d.c линкуется
   в программу (cube3d), ядро о ней не знает. Без libc, без кучи: всю память движок берёт из куска, который даёт программа.

   Устройство (в духе мобильных GPU, "tile-based"):
     1. Программа пишет КОМАНДЫ в компактный буфер слов (k3d_cb_*): матрицы, состояние, привязка ресурсов, рисование.
        В буфере нет указателей - только числа и номера ресурсов (вершины, индексы, текстуры создаются отдельно и
        получают номера). Тот же поток слов можно отправить аппаратному GPU-пути: он принимает тот же формат.
     2. k3d_submit выполняет поток. Геометрия (вершины, свет по вершинам, отсечение по пирамиде видимости, настройка
        треугольников) считается одним потоком, треугольники раскладываются по плиткам экрана 32x32.
     3. Плитки растеризуются ПАРАЛЛЕЛЬНО потоками процесса (futex). Плитку целиком ведёт один поток, треугольники в ней идут
        в порядке подачи - результат не зависит от числа потоков и порядка плиток (проверено хост-тестом побитно).
   Растеризация: центры пикселей, правило "верх-лево" на фиксированной точке 28.4 (общие рёбра не дают ни дыр, ни двойного
   покрытия), буфер глубины float, перспективно-верная интерполяция цвета, текстурных координат и нормалей, билинейная
   выборка с выбором мип-уровня на пиксель, свет Ламберта по вершинам или по пикселям, прозрачность (смешивание в
   порядке подачи; сначала все непрозрачные треугольники плитки, потом прозрачные). Порядок "непрозрачные, потом прозрачные"
   действует внутри одного сброса плиток (k3d_stats.flushes): держите max_tris больше числа треугольников кадра, и сброс будет один.

   Система координат: левая, камера смотрит в +z, +x вправо, +y вверх. Лицевая грань - обход против часовой стрелки при
   взгляде на неё (как её видит камера, y вверх). Матрицы - строками (m[строка*4+столбец]), вектор - столбец: v' = M * v.
   Глубина 0 (ближняя плоскость) .. 1 (дальняя). Цвета: 0xAARRGGBB. */

typedef unsigned char k3d_u8;
typedef unsigned short k3d_u16;
typedef unsigned int k3d_u32;
typedef unsigned long long k3d_u64;

enum { K3D_OK = 0, K3D_E_ARG = -1, K3D_E_NOMEM = -2, K3D_E_FULL = -3, K3D_E_BADCMD = -4, K3D_E_HANDLE = -5, K3D_E_RANGE = -6 };

#define K3D_TILE        32          /* сторона плитки, пикселей */
#define K3D_MAX_WORKERS 8           /* потоков-помощников; сам вызывающий поток тоже растеризует */
#define K3D_MAX_RES     32          /* ресурсов каждого вида */
#define K3D_MAX_MIPS    11

/* ---- математика (без libm) ---- */
typedef struct { float m[16]; } k3d_mat4;
float k3d_sinf(float x);            /* ряд Тейлора, погрешность < 3e-6 на любых углах */
float k3d_cosf(float x);
float k3d_sqrtf(float x);
k3d_mat4 k3d_mat4_identity(void);
k3d_mat4 k3d_mat4_mul(const k3d_mat4* a, const k3d_mat4* b);     /* a * b: сначала b, потом a */
k3d_mat4 k3d_mat4_translate(float x, float y, float z);
k3d_mat4 k3d_mat4_scale(float x, float y, float z);
k3d_mat4 k3d_mat4_rot_x(float a);
k3d_mat4 k3d_mat4_rot_y(float a);
k3d_mat4 k3d_mat4_rot_z(float a);
/* f = 1/tan(fov_y/2); глубина zn -> 0, zf -> 1 */
k3d_mat4 k3d_mat4_perspective(float f, float aspect, float zn, float zf);

/* ---- вершина ---- */
typedef struct {
    float x, y, z;                  /* положение в пространстве модели */
    float nx, ny, nz;               /* нормаль (для света) */
    float u, v;                     /* текстурные координаты (повторяются или зажимаются по состоянию) */
    k3d_u32 color;                  /* 0xAARRGGBB, множитель цвета; альфа нужна только при K3D_BLEND */
} k3d_vertex_t;

/* ---- состояние рисования (слово flags команды STATE) ---- */
enum {
    K3D_CULL_NONE = 0, K3D_CULL_BACK = 1, K3D_CULL_FRONT = 2,       /* биты 0..1 */
    K3D_DEPTH_TEST = 1u << 2,       /* пиксель виден, если ближе записанного */
    K3D_DEPTH_WRITE = 1u << 3,
    K3D_BLEND = 1u << 4,            /* прозрачность: альфа вершин * альфа текстуры * альфа tint */
    K3D_TEXTURE = 1u << 5,          /* умножать на привязанную текстуру */
    K3D_LIGHT_VERTEX = 1u << 6,     /* свет Ламберта в вершинах (цвет интерполируется) */
    K3D_LIGHT_PIXEL = 1u << 7,      /* свет Ламберта в пикселях (нормаль интерполируется и нормируется) */
    K3D_FILTER_BILINEAR = 1u << 8,  /* билинейная выборка (иначе ближайший тексель) */
    K3D_WRAP_CLAMP = 1u << 9,       /* зажимать координаты к краю (иначе повторять) */
};
#define K3D_STATE_DEFAULT (K3D_CULL_BACK | K3D_DEPTH_TEST | K3D_DEPTH_WRITE)

/* ---- команды: формат потока ----
   Слово 0 команды: (код << 16) | длина в словах (вместе с этим словом). Числа с плавающей точкой - битами float.
   В начале каждого k3d_submit состояние сбрасывается: матрицы единичные, flags = K3D_STATE_DEFAULT, tint = 0xFFFFFFFF,
   свет выключен (ambient 1, остальное 0), ничего не привязано - поток самодостаточен и переносим.
     CLEAR  (1) [верхний цвет, нижний цвет, глубина] - вертикальный градиент и глубина; выполняется перед следующим рисованием
     MATRIX (2) [слот 0 модель / 1 вид / 2 проекция, 16 float строками]
     STATE  (3) [flags, tint 0xAARRGGBB - множитель цвета и альфы]
     LIGHT  (4) [направление К свету (мировое) x y z, цвет света r g b, ambient r g b] (float, 0..1)
     BIND   (5) [вид 0 вершины / 1 индексы / 2 текстура, номер ресурса или 0xFFFFFFFF - отвязать]
     DRAW   (6) [первый индекс, число индексов (кратно 3), смещение вершины] - треугольники из привязанных буферов */
enum { K3D_CMD_CLEAR = 1, K3D_CMD_MATRIX = 2, K3D_CMD_STATE = 3, K3D_CMD_LIGHT = 4, K3D_CMD_BIND = 5, K3D_CMD_DRAW = 6 };
enum { K3D_SLOT_MODEL = 0, K3D_SLOT_VIEW = 1, K3D_SLOT_PROJ = 2 };
enum { K3D_BIND_VERTEX = 0, K3D_BIND_INDEX = 1, K3D_BIND_TEXTURE = 2 };
#define K3D_NONE 0xFFFFFFFFu

typedef struct { k3d_u32* w; k3d_u32 n, cap; int err; } k3d_cmdbuf_t;   /* err: первая ошибка (переполнение) - дальше записи игнорируются */
void k3d_cb_init(k3d_cmdbuf_t* cb, k3d_u32* mem, k3d_u32 cap_words);
void k3d_cb_reset(k3d_cmdbuf_t* cb);
int k3d_cb_clear(k3d_cmdbuf_t* cb, k3d_u32 top, k3d_u32 bottom, float depth);
int k3d_cb_matrix(k3d_cmdbuf_t* cb, int slot, const k3d_mat4* m);
int k3d_cb_state(k3d_cmdbuf_t* cb, k3d_u32 flags, k3d_u32 tint);
int k3d_cb_light(k3d_cmdbuf_t* cb, const float dir[3], const float color[3], const float ambient[3]);
int k3d_cb_bind(k3d_cmdbuf_t* cb, int kind, int handle);
int k3d_cb_draw(k3d_cmdbuf_t* cb, k3d_u32 first_index, k3d_u32 index_count, k3d_u32 base_vertex);
/* Проверка потока без выполнения: длины, коды, слоты. 0 или K3D_E_BADCMD. k3d_submit вызывает её сам. */
int k3d_cb_validate(const k3d_u32* w, k3d_u32 n);

/* ---- платформа: потоки и futex (на Kira - k_thread_create/k_futex_*, в хост-тесте - pthread) ---- */
typedef struct {
    /* запустить поток fn(arg) на стеке [stack, stack+size); 0 - успех */
    int (*spawn)(void (*fn)(k3d_u64), void* stack, k3d_u64 size, k3d_u64 arg);
    void (*futex_wait)(volatile k3d_u32* w, k3d_u32 expected);      /* спать, пока *w == expected (может вернуться раньше) */
    void (*futex_wake)(volatile k3d_u32* w, k3d_u32 n);
    k3d_u64 (*clock_ns)(void);                                      /* можно 0: тогда времена в статистике нули */
} k3d_sys_t;

typedef struct {
    int width, height;              /* размер цели */
    int workers;                    /* потоков-помощников 0..K3D_MAX_WORKERS (0 - всё в вызывающем потоке) */
    k3d_u32 max_tris;               /* треугольников между сбросами (при переполнении движок сам "сбрасывает" плитки) */
    k3d_u32 max_bins;               /* записей "треугольник в плитке" между сбросами; не меньше числа плиток */
    k3d_u32 max_states;             /* снимков состояния между сбросами */
    k3d_u32 max_verts;              /* вершин в одном буфере */
    k3d_u32 res_bytes;              /* память под вершины, индексы и текстуры (с мип-уровнями) */
    k3d_u32 stack_bytes;            /* стек каждого помощника */
    k3d_u32 par_min_cost;           /* помощников будят, только если оценка работы сброса (примерно нс на ядре 3 ГГц) не меньше; 0 - всегда. Разбудить потоки и дождаться их стоит ~100-200 мкс: на мелких кадрах быстрее одному */
    k3d_sys_t sys;
} k3d_config_t;
/* Разумные значения для окна w x h (потоки: workers, sys заполняет программа). */
void k3d_config_default(k3d_config_t* cfg, int w, int h, int workers);
/* Сколько байт памяти нужно движку при такой конфигурации (0 - конфигурация неверна). */
k3d_u64 k3d_mem_size(const k3d_config_t* cfg);

typedef struct k3d_ctx k3d_ctx;
/* mem - выровнена на 16, не меньше k3d_mem_size. NULL - неверная конфигурация или мало памяти. Если потоков запустилось меньше
   просимого, движок работает с теми, что есть (k3d_workers). */
k3d_ctx* k3d_create(const k3d_config_t* cfg, void* mem, k3d_u64 bytes);
void k3d_destroy(k3d_ctx* c);       /* останавливает потоки (ждёт их выхода); память отдаёт программе */
int k3d_workers(const k3d_ctx* c);

/* ---- ресурсы (копируются в память движка; номер >= 0 или код ошибки) ---- */
int k3d_vbuf_create(k3d_ctx* c, const k3d_vertex_t* v, k3d_u32 n);
int k3d_ibuf_create(k3d_ctx* c, const k3d_u16* idx, k3d_u32 n);
/* w и h - степени двойки (1..1024); pixels 0xAARRGGBB; mips - построить уменьшенные уровни (усреднение 2x2) */
int k3d_tex_create(k3d_ctx* c, int w, int h, const k3d_u32* pixels, int mips);
void k3d_res_reset(k3d_ctx* c);     /* освободить все ресурсы (номера недействительны) */

typedef struct {
    k3d_u64 frames;                 /* выполненных k3d_submit за жизнь */
    k3d_u32 tris_in;                /* треугольников на входе (из DRAW) */
    k3d_u32 tris_culled;            /* отброшено по обходу */
    k3d_u32 tris_outside;           /* целиком вне пирамиды видимости (или вырожденных) */
    k3d_u32 tris_clipped;           /* потребовали отсечения */
    k3d_u32 tris_drawn;             /* ушло в растеризацию (после отсечения может быть больше, чем на входе) */
    k3d_u32 flushes;                /* сколько раз плитки растеризовались за вызов (>1 - переполнение пулов) */
    k3d_u32 flushes_parallel;       /* из них - с помощниками (остальные - в вызывающем потоке: работы мало) */
    k3d_u32 bin_entries;            /* записей в списках плиток */
    k3d_u32 px_written;             /* записано пикселей цвета */
    k3d_u64 ns_geometry, ns_raster; /* только при sys.clock_ns */
} k3d_stats_t;

/* Выполнить поток команд над целью target (width x height пикселей 0x00RRGGBB, строка pitch пикселей). Состояние
   по вызову начинается заново. При ошибке в потоке ничего не рисуется, если он не прошёл проверку; ошибка в
   рисовании (неверный номер, индекс за границей) - то, что было до неё, дорисовывается и возвращается код. Буфер глубины
   живёт между вызовами: без CLEAR следующий вызов рисует поверх с учётом прежней глубины. */
int k3d_submit(k3d_ctx* c, const k3d_u32* words, k3d_u32 n, k3d_u32* target, int pitch);
const k3d_stats_t* k3d_stats(const k3d_ctx* c);        /* статистика последнего k3d_submit (frames - накопительно) */
const float* k3d_depth(const k3d_ctx* c);              /* буфер глубины width*height (для тестов и отладки) */
#endif
