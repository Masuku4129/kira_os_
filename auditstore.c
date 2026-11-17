/* Журнал аудита на диске: см. auditstore.h и docs/AUDIT.md. */
#include "auditstore.h"
#include "kfs.h"
#include "sha256.h"
#include "crypto.h"

static void mcpy(void* d, const void* s, size_t n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; }
static void mzero(void* d, size_t n) { uint8_t* a = d; while (n--) *a++ = 0; }
static int  meq(const void* x, const void* y, size_t n) { return crypto_eq(x, y, n); }

static void seg_path(char* o, uint64_t first) {
    const char* p = "/var/audit."; unsigned n = 0;
    while (*p) o[n++] = *p++;
    for (int i = 9; i >= 0; i--) { o[n + (unsigned)i] = (char)('0' + first % 10); first /= 10; }
    n += 10;
    const char* e = ".log"; while (*e) o[n++] = *e++;
    o[n] = 0;
}
static void vfail(aud_vstate_t* v, int code, uint64_t seq) { if (v->code == AUD_V_OK) { v->code = code; v->bad_seq = seq; v->bad_off = 0; v->bad_file = v->files > 0 ? v->files - 1 : 0; } }

/* Читает файл целиком (не больше cap). Результат: длина, -1 - файла нет, -2 - ошибка чтения; *big = 1, если файл длиннее cap. */
static int64_t read_file(const char* path, uint8_t* buf, size_t cap, int* big) {
    kfs_stat_t st; *big = 0;
    int r = kfs_stat(path, &st);
    if (r == KFS_ERR_NOENT) return -1;
    if (r != 0 || st.type != KFS_FILE) return -2;
    size_t want = st.size;
    if (want > cap) { want = cap; *big = 1; }
    if (want == 0) return 0;
    int64_t g = kfs_read(path, 0, buf, want);
    return g == (int64_t)want ? g : -2;
}

typedef struct { uint64_t* a; unsigned n; unsigned over; } listctx_t;
static int list_cb(void* ctx, const char* name, const kfs_stat_t* st) {
    listctx_t* c = ctx; (void)st;
    const char* p = "audit.";
    unsigned i = 0;
    while (p[i] && name[i] == p[i]) i++;
    if (p[i]) return 0;
    uint64_t v = 0; unsigned d = 0;
    while (name[i] >= '0' && name[i] <= '9') { v = v * 10 + (uint64_t)(name[i] - '0'); i++; d++; }
    if (d != 10 || name[i] != '.' || name[i + 1] != 'l' || name[i + 2] != 'o' || name[i + 3] != 'g' || name[i + 4]) return 0;
    if (c->n >= AUD_SEGMAX) { c->over++; return 0; }
    c->a[c->n++] = v;
    return 0;
}

static void sort_u64(uint64_t* a, unsigned n) { for (unsigned i = 1; i < n; i++) { uint64_t k = a[i]; unsigned j = i; while (j && a[j - 1] > k) { a[j] = a[j - 1]; j--; } a[j] = k; } }

static const aud_rec_t* rec_at(const uint8_t* seg, size_t i) { return (const aud_rec_t*)(seg + AUD_HDR_SIZE + i * AUD_REC_SIZE); }

static void cur_fresh(aud_store_t* s, uint64_t first, const uint8_t anchor[32], const aud_rec_t* anchor_ckpt) {
    aud_hdr_t h; aud_hdr_make(&h, first, s->pub, anchor_ckpt);
    if (anchor) mcpy(h.anchor, anchor, 32);
    mcpy(s->cur, &h, sizeof h); s->cur_len = AUD_HDR_SIZE; s->cur_first = first;
    s->active_dirty = 1;
}

static void note_last(aud_store_t* s, const uint8_t* seg, size_t len) {            /* запомнить последнюю запись файла (якорь следующего сегмента) */
    size_t n = len >= AUD_HDR_SIZE ? (len - AUD_HDR_SIZE) / AUD_REC_SIZE : 0;
    if (!n) return;
    const aud_rec_t* r = rec_at(seg, n - 1);
    if (r->kind == AUD_K_CKPT) { mcpy(&s->last_ckpt, r, sizeof *r); s->have_ckpt = 1; } else s->have_ckpt = 0;
}

static void evidence_path(char* o, uint64_t seq) { seg_path(o, seq); unsigned n = 0; while (o[n]) n++; n -= 4; o[n++] = '.'; o[n++] = 'b'; o[n++] = 'a'; o[n++] = 'd'; o[n] = 0; }   /* /var/audit.NNNNNNNNNN.bad: улика, не файл журнала */

static int hdr_ok(const uint8_t* b, size_t len) {
    const aud_hdr_t* h = (const aud_hdr_t*)b;
    return len >= AUD_HDR_SIZE && meq(h->magic, "KIRAAUD1", 8) && h->version == 1 && h->rec_size == AUD_REC_SIZE && h->first_seq != 0;
}

/* Общий проход по файлам диска. resume = 1 при загрузке: подготавливает продолжение цепочки; 0 - только проверка (буферы хозяина не трогает). */
static int scan_disk(aud_store_t* s, aud_vstate_t* v, int resume) {
    uint64_t firsts[AUD_SEGMAX]; listctx_t lc = { firsts, 0, 0 };
    if (kfs_list(AUD_DIR, list_cb, &lc) != 0) return -1;
    sort_u64(firsts, lc.n);
    int big;
    uint8_t* tmp = s->tmp;
    int64_t cl = read_file(AUD_CKPTFILE, tmp, AUD_REC_SIZE + 1, &big);        /* свидетель: последняя подписанная точка */
    if (cl == AUD_REC_SIZE && !big) {
        const aud_rec_t* w = (const aud_rec_t*)tmp;
        uint8_t kid[16]; aud_keyid(s->pub, kid);
        if (w->kind == AUD_K_CKPT && !meq(kid, ((const aud_ckpt_t*)w)->keyid, 16)) vfail(v, AUD_V_KEY, w->seq);
        else if (w->kind == AUD_K_CKPT && aud_ckpt_check(w, s->pub) == 0) { aud_vstate_want(v, w); if (resume) s->ckpt_file_seq = w->seq; }
        else vfail(v, AUD_V_SIG, w->seq);
    } else if (cl >= 0) vfail(v, AUD_V_FORMAT, 0);
    else if (cl == -2) return -1;
    if (resume && lc.n + 1 > 16) v->sig_from = firsts[lc.n - 8];             /* при загрузке много сегментов: подписи проверяются у последних восьми (цепочка - у всех); `audit verify` проверяет все */
    for (unsigned i = 0; i < lc.n; i++) {
        char p[40]; seg_path(p, firsts[i]);
        int64_t n = read_file(p, tmp, AUD_SEGBYTES, &big);
        if (n == -2) return -1;
        if (n < 0) continue;
        if (big) { v->files++; vfail(v, AUD_V_FORMAT, firsts[i]); continue; }
        aud_verify_file(v, tmp, (size_t)n, s->pub);
        if (resume) note_last(s, tmp, (size_t)n);
    }
    int64_t an = read_file(AUD_ACTIVE, tmp, AUD_SEGBYTES, &big);
    if (an == -2) return -1;
    int active_used = 0, bad_active = 0;
    if (an >= 0) {
        if (big || !hdr_ok(tmp, (size_t)an)) { v->files++; vfail(v, AUD_V_FORMAT, v->next_seq); bad_active = 1; }
        else if (lc.n && ((const aud_hdr_t*)tmp)->first_seq == firsts[lc.n - 1]) {     /* обрыв между записью закрытого файла и нового текущего: текущий - начало закрытого */
            char p[40]; seg_path(p, firsts[lc.n - 1]);
            int64_t rn = read_file(p, s->tmp2, AUD_SEGBYTES, &big);
            size_t m = (size_t)an < (size_t)(rn < 0 ? 0 : rn) ? (size_t)an : (size_t)(rn < 0 ? 0 : rn), k = 0;
            while (k < m && s->tmp2[k] == tmp[k]) k++;
            if (k < m) { v->files++; vfail(v, AUD_V_CHAIN, firsts[lc.n - 1] + (k < AUD_HDR_SIZE ? 0 : (k - AUD_HDR_SIZE) / AUD_REC_SIZE)); }
            else if ((size_t)an > (size_t)(rn < 0 ? 0 : rn)) { v->files++; vfail(v, AUD_V_FORMAT, firsts[lc.n - 1]); }
        } else { aud_verify_file(v, tmp, (size_t)an, s->pub); active_used = 1; }
    }
    aud_verify_end(v);
    if (!resume) return 0;
    /* Продолжение: цепочка - от последней записи, фактически лежащей на диске. */
    s->nsegs = lc.n; for (unsigned i = 0; i < lc.n; i++) s->segs[i] = firsts[i];
    s->next_seq = v->started ? v->next_seq : 1;
    mcpy(s->tail_head, v->head, 32);
    s->closed_len = 0; s->cur_len = 0;
    int trailing = active_used && ((size_t)an - AUD_HDR_SIZE) % AUD_REC_SIZE;
    if (bad_active || trailing) {                                            /* то, что при продолжении не сохранится, остаётся уликой */
        char ev[48]; evidence_path(ev, s->next_seq);
        if (kfs_clone(AUD_ACTIVE, ev) != 0) s->io_errors++;
    }
    if (active_used) {
        size_t nrec = ((size_t)an - AUD_HDR_SIZE) / AUD_REC_SIZE;
        mcpy(s->cur, tmp, AUD_HDR_SIZE + nrec * AUD_REC_SIZE); s->cur_len = AUD_HDR_SIZE + nrec * AUD_REC_SIZE; s->cur_first = ((const aud_hdr_t*)tmp)->first_seq;
        if (nrec) { mcpy(&s->last_ckpt, rec_at(s->cur, nrec - 1), sizeof(aud_rec_t)); s->have_ckpt = s->last_ckpt.kind == AUD_K_CKPT; }
        if (trailing) s->active_dirty = 1;
        if (nrec >= AUD_SEG_RECS) {                                          /* полный сегмент лежит как текущий: он закрывается и пишется как закрытый */
            mcpy(s->closed, s->cur, s->cur_len); s->closed_len = s->cur_len; s->closed_first = s->cur_first;
            cur_fresh(s, s->next_seq, s->tail_head, s->have_ckpt ? &s->last_ckpt : 0);
        }
    } else {
        cur_fresh(s, s->next_seq, v->started ? v->head : 0, s->have_ckpt && s->last_ckpt.seq + 1 == s->next_seq ? &s->last_ckpt : 0);
    }
    s->flushed_next = s->next_seq;
    mcpy(s->flushed_head, s->tail_head, 32);
    return 0;
}

int aud_store_load(aud_store_t* s, const uint8_t pub[32], aud_vstate_t* v, aud_chain_t* chain) {
    mzero(s, sizeof *s);
    mcpy(s->pub, pub, 32);
    aud_vstate_init(v);
    kfs_stat_t st;
    if (kfs_stat(AUD_DIR, &st) != 0) { if (kfs_mkdir(AUD_DIR) != 0) return -1; }
    if (scan_disk(s, v, 1) != 0) return -1;
    aud_chain_init(chain, s->next_seq, v->started ? v->head : 0);
    return 0;
}

static int write_ckpt_file(aud_store_t* s, const uint8_t* seg, size_t len) {
    size_t n = len >= AUD_HDR_SIZE ? (len - AUD_HDR_SIZE) / AUD_REC_SIZE : 0;
    for (size_t i = n; i-- > 0;) {
        const aud_rec_t* r = rec_at(seg, i);
        if (r->kind == AUD_K_CKPT) {
            if (r->seq <= s->ckpt_file_seq) return 0;
            if (kfs_write(AUD_CKPTFILE, r, AUD_REC_SIZE) != 0) return -1;
            s->ckpt_file_seq = r->seq; return 0;
        }
    }
    return 0;
}

int aud_store_append(aud_store_t* s, const aud_rec_t* r) {
    if (r->seq != s->next_seq) return -1;
    if (s->closed_len) return -1;                                          /* закрытый сегмент ждёт записи на диск */
    size_t n = (s->cur_len - AUD_HDR_SIZE) / AUD_REC_SIZE;
    if (n >= AUD_SEG_RECS) return -1;
    mcpy(s->cur + s->cur_len, r, AUD_REC_SIZE); s->cur_len += AUD_REC_SIZE;
    aud_hash(r, s->tail_head);
    s->next_seq++;
    s->active_dirty = 1;
    if (r->kind == AUD_K_CKPT) { mcpy(&s->last_ckpt, r, sizeof *r); s->have_ckpt = 1; } else s->have_ckpt = 0;
    if (r->seq % AUD_SEG_RECS == 0) {                                      /* сегмент закрыт: уходит в закрытый буфер, начинается новый с якорем - этой точкой */
        mcpy(s->closed, s->cur, s->cur_len); s->closed_len = s->cur_len; s->closed_first = s->cur_first;
        cur_fresh(s, r->seq + 1, s->tail_head, r);
    }
    return 0;
}

int aud_store_room(const aud_store_t* s) { return !s->closed_len && (s->cur_len - AUD_HDR_SIZE) / AUD_REC_SIZE < AUD_SEG_RECS; }

int aud_store_flush(aud_store_t* s) {
    if (s->closed_len) {
        char p[40]; seg_path(p, s->closed_first);
        if (kfs_write(p, s->closed, s->closed_len) != 0) { s->io_errors++; return -1; }
        if (s->nsegs < AUD_SEGMAX) s->segs[s->nsegs++] = s->closed_first;
        size_t n = (s->closed_len - AUD_HDR_SIZE) / AUD_REC_SIZE;
        s->flushed_next = s->closed_first + n;
        aud_hash(rec_at(s->closed, n - 1), s->flushed_head);
        (void)write_ckpt_file(s, s->closed, s->closed_len);
        s->closed_len = 0;
        s->active_dirty = 1;
    }
    if (s->active_dirty) {
        if (kfs_write(AUD_ACTIVE, s->cur, s->cur_len) != 0) { s->io_errors++; return -1; }
        size_t n = (s->cur_len - AUD_HDR_SIZE) / AUD_REC_SIZE;
        s->flushed_next = s->cur_first + n;
        if (n) aud_hash(rec_at(s->cur, n - 1), s->flushed_head); else mcpy(s->flushed_head, ((const aud_hdr_t*)s->cur)->anchor, 32);
        s->active_dirty = 0;
        if (write_ckpt_file(s, s->cur, s->cur_len) != 0) { s->io_errors++; return -1; }
    }
    s->io_errors = 0;
    return 0;
}

int aud_store_verify(aud_store_t* s, aud_vstate_t* v) {
    aud_vstate_init(v);
    if (scan_disk(s, v, 0) != 0) return -1;
    if (v->code == AUD_V_OK && s->flushed_next > 1) {
        if (v->next_seq < s->flushed_next) vfail(v, AUD_V_TRUNC, v->next_seq);                 /* на диске меньше, чем записано */
        else if (v->next_seq == s->flushed_next && !meq(v->head, s->flushed_head, 32)) vfail(v, AUD_V_CHAIN, s->flushed_next - 1);
    }
    return 0;
}

int aud_store_read_rec(aud_store_t* s, uint64_t seq, aud_rec_t* out) {
    if (seq >= s->cur_first) {
        size_t n = (s->cur_len - AUD_HDR_SIZE) / AUD_REC_SIZE;
        if (seq - s->cur_first < n) { mcpy(out, rec_at(s->cur, seq - s->cur_first), AUD_REC_SIZE); return 0; }
        return -1;
    }
    if (s->closed_len && seq >= s->closed_first) {
        size_t n = (s->closed_len - AUD_HDR_SIZE) / AUD_REC_SIZE;
        if (seq - s->closed_first < n) { mcpy(out, rec_at(s->closed, seq - s->closed_first), AUD_REC_SIZE); return 0; }
        return -1;
    }
    for (unsigned i = s->nsegs; i-- > 0;) {
        if (s->segs[i] > seq) continue;
        char p[40]; seg_path(p, s->segs[i]); int big;
        int64_t n = read_file(p, s->tmp, AUD_SEGBYTES, &big);
        if (n < (int64_t)AUD_HDR_SIZE) return -1;
        size_t cnt = ((size_t)n - AUD_HDR_SIZE) / AUD_REC_SIZE;
        if (seq - s->segs[i] >= cnt) return -1;
        mcpy(out, rec_at(s->tmp, seq - s->segs[i]), AUD_REC_SIZE);
        return 0;
    }
    return -1;
}

int aud_store_prune_due(const aud_store_t* s, uint64_t* upto) {
    if (s->nsegs <= AUD_MAX_SEGS) return 0;
    *upto = s->segs[1] - 1;                                                  /* самый старый закрытый сегмент кончается перед следующим */
    return 1;
}

unsigned aud_store_prune(aud_store_t* s, uint64_t upto) {
    unsigned gone = 0;
    while (s->nsegs > 1 && s->segs[1] - 1 <= upto) {
        char p[40]; seg_path(p, s->segs[0]);
        if (kfs_unlink(p) != 0) break;
        for (unsigned i = 1; i < s->nsegs; i++) s->segs[i - 1] = s->segs[i];
        s->nsegs--; gone++;
    }
    return gone;
}
