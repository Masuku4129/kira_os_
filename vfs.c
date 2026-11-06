#include "vfs.h"

#ifdef KIRA_HOST_TEST
typedef int vlock_t;
#define VL_INIT 0
static void vl_lock(vlock_t* l) { (void)l; }
static void vl_unlock(vlock_t* l) { (void)l; }
static int vl_trylock(vlock_t* l) { (void)l; return 1; }
#else
#include "kmutex.h"
typedef kmutex_t vlock_t;
#define VL_INIT KMUTEX_INIT
static void vl_lock(vlock_t* l) { kmutex_lock(l); }
static void vl_unlock(vlock_t* l) { kmutex_unlock(l); }
static int vl_trylock(vlock_t* l) { return kmutex_trylock(l); }
#endif

static struct { int used, read_only; fat_vol_t* vol; char desc[48]; } mnt[VFS_MAX];
static vlock_t lk[VFS_MAX] = { VL_INIT, VL_INIT, VL_INIT, VL_INIT };
static volatile uint32_t gen;

static int mount_fat(fat_vol_t* vol, const char* desc, int read_only) {
    if (!vol || !desc || !vol->mounted) return -1;
    for (int i = 0; i < VFS_MAX; i++) {
        vl_lock(&lk[i]);
        if (!mnt[i].used) {
            mnt[i].vol = vol; mnt[i].used = 1; mnt[i].read_only = read_only;
            int k = 0; while (desc[k] && k < 47) { mnt[i].desc[k] = desc[k]; k++; } mnt[i].desc[k] = 0;
            vl_unlock(&lk[i]);
            __atomic_add_fetch(&gen, 1, __ATOMIC_RELEASE);
            return i;
        }
        vl_unlock(&lk[i]);
    }
    return -1;
}
int vfs_mount_fat(fat_vol_t* vol, const char* desc) { return mount_fat(vol, desc, 0); }
int vfs_mount_fat_ro(fat_vol_t* vol, const char* desc) { return mount_fat(vol, desc, 1); }

void vfs_unmount(int n) {
    if (n < 0 || n >= VFS_MAX) return;
    vl_lock(&lk[n]);                                  /* дождаться, пока операция с томом закончится */
    mnt[n].used = 0; mnt[n].read_only = 0; mnt[n].vol = 0;
    vl_unlock(&lk[n]);
    __atomic_add_fetch(&gen, 1, __ATOMIC_RELEASE);
}

int vfs_unmount_try(int n) {
    if (n < 0 || n >= VFS_MAX) return 0;
    if (!vl_trylock(&lk[n])) return -1;
    mnt[n].used = 0; mnt[n].vol = 0;
    vl_unlock(&lk[n]);
    __atomic_add_fetch(&gen, 1, __ATOMIC_RELEASE);
    return 0;
}

int vfs_flush(int n) {
    if (n < 0 || n >= VFS_MAX) return KFS_ERR_INVAL;
    vl_lock(&lk[n]);
    int r = !mnt[n].used ? KFS_ERR_NOFS : mnt[n].read_only ? KFS_OK : fat_flush(mnt[n].vol);
    vl_unlock(&lk[n]);
    return r;
}

uint32_t vfs_generation(void) { return __atomic_load_n(&gen, __ATOMIC_ACQUIRE); }

int vfs_mounts(vfs_info_t* out, int max) {
    int k = 0;
    for (int i = 0; i < VFS_MAX && k < max; i++) {
        vl_lock(&lk[i]);
        if (mnt[i].used) {
            vfs_info_t* o = &out[k++];
            o->name[0] = 'u'; o->name[1] = 's'; o->name[2] = 'b'; o->name[3] = (char)('0' + i); o->name[4] = 0;
            for (int j = 0; j < 12; j++) o->label[j] = mnt[i].vol->label[j];
            for (int j = 0; j < 48; j++) o->desc[j] = mnt[i].desc[j];
            fat_info(mnt[i].vol, &o->total_kb, &o->free_kb);
            o->fat_type = mnt[i].vol->type; o->read_only = mnt[i].read_only;
        }
        vl_unlock(&lk[i]);
    }
    return k;
}

int vfs_any(void) {
    if (kfs_mounted()) return 1;
    for (int i = 0; i < VFS_MAX; i++) if (mnt[i].used) return 1;
    return 0;
}

/* "/usbN" или "/usbN/..." -> номер тома и путь внутри; -1 - путь KiraFS. */
static int route(const char* p, const char** rest) {
    if (p[0] != '/' || p[1] != 'u' || p[2] != 's' || p[3] != 'b' || p[4] < '0' || p[4] >= '0' + VFS_MAX || (p[5] && p[5] != '/')) return -1;
    *rest = p[5] ? p + 5 : "/";
    return p[4] - '0';
}

#define ON_VOL(call_kfs, call_fat) do {                                              \
    const char* rest; int n = route(path, &rest);                                     \
    if (n < 0) return call_kfs;                                                       \
    vl_lock(&lk[n]);                                                                  \
    __typeof__(call_kfs) r = mnt[n].used ? call_fat : KFS_ERR_NOFS;                   \
    vl_unlock(&lk[n]);                                                                \
    return r;                                                                         \
} while (0)

int vfs_stat(const char* path, kfs_stat_t* st) {
    const char* rest; int n = route(path, &rest);
    if (n < 0 && path[0] == '/' && !path[1] && !kfs_mounted()) { st->type = KFS_DIR; st->size = 0; st->ino = 0; return 0; }
    ON_VOL(kfs_stat(path, st), fat_stat(mnt[n].vol, rest, st));
}
int64_t vfs_read(const char* path, uint64_t off, void* buf, size_t len) { ON_VOL(kfs_read(path, off, buf, len), fat_read(mnt[n].vol, rest, off, buf, len)); }
int vfs_write(const char* path, const void* data, size_t len) {
    const char* rest; int n = route(path, &rest);
    if (n < 0) return kfs_write(path, data, len);
    vl_lock(&lk[n]);
    int r = !mnt[n].used ? KFS_ERR_NOFS : mnt[n].read_only ? KFS_ERR_RO : fat_write(mnt[n].vol, rest, data, len);
    vl_unlock(&lk[n]);
    return r;
}
int vfs_mkdir(const char* path) {
    const char* rest; int n = route(path, &rest);
    if (n < 0) return kfs_mkdir(path);
    vl_lock(&lk[n]);
    int r = !mnt[n].used ? KFS_ERR_NOFS : mnt[n].read_only ? KFS_ERR_RO : fat_mkdir(mnt[n].vol, rest);
    vl_unlock(&lk[n]);
    return r;
}
int vfs_unlink(const char* path) {
    const char* rest; int n = route(path, &rest);
    if (n < 0) return kfs_unlink(path);
    if (rest[0] == '/' && !rest[1]) return KFS_ERR_INVAL;
    vl_lock(&lk[n]);
    int r = !mnt[n].used ? KFS_ERR_NOFS : mnt[n].read_only ? KFS_ERR_RO : fat_unlink(mnt[n].vol, rest);
    vl_unlock(&lk[n]);
    return r;
}

int vfs_list(const char* path, kfs_list_cb cb, void* ctx) {
    const char* rest; int n = route(path, &rest);
    if (n >= 0) {
        vl_lock(&lk[n]);
        int r = mnt[n].used ? fat_list(mnt[n].vol, rest, cb, ctx) : KFS_ERR_NOFS;
        vl_unlock(&lk[n]);
        return r;
    }
    int root = path[0] == '/' && !path[1];
    int r = kfs_mounted() ? kfs_list(path, cb, ctx) : (root ? 0 : KFS_ERR_NOFS);
    if (r || !root) return r;
    for (int i = 0; i < VFS_MAX; i++) {                                    /* тома - каталоги в корне */
        if (!mnt[i].used) continue;
        char name[8] = { 'u', 's', 'b', (char)('0' + i), 0 };
        kfs_stat_t st = { KFS_DIR, 0, 0 };
        if (cb(ctx, name, &st)) break;
    }
    return 0;
}
