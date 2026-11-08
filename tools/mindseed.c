// Семя опыта Разума: файл опыта (/var/mind.dat), снятый с диска образа после тренажёра mind-train, -> data/mind_seed.bin.
//   mindseed вход.dat выход.bin
// В семя попадают только веса модели предвидения (число наблюдений урезано до MIND_SEED_N) и подсказки по рычагам
// (mind_model_seed - тот же код, что читает семя в ядре); нормы устройства, квант, профили, часы - нет.
// Опыт короче 10 минут жизни не годится: семя из него было бы шумом.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mind.h"

static unsigned char* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fprintf(stderr, "%s: размер не определён\n", p); exit(2); }
    unsigned char* b = malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "%s: не прочитан\n", p); exit(2); }
    fclose(f); *n = (size_t)len; return b;
}

/* Испытательный образец для сценария стенда mind-seed: НЕ результат обучения, а файл с известными числами, по которым
   стенд проверяет, что ядро читает семя и берёт из него только веса и подсказки. */
static int fixture(const char* out) {
    static mind_t m;
    mind_init(&m, 0, 4);
    m.m.lifetime_s = 3600; m.m.net.n = 3000; m.m.net.pos = 200; m.m.net.w[3] = 12345;
    m.m.norm[MS_LOAD].n = 3600; m.m.norm[MS_LOAD].mean_x100 = 4200; m.m.q_best[2] = 12;
    m.m.lv[ML_AUDIO_LEAD].best = 1; m.m.lv[ML_AUDIO_LEAD].wins = 3; m.m.lv[ML_AUDIO_LEAD].trials = 5;
    mind_model_t src = m.m, seed;
    mind_model_seal(&src);
    if (!mind_model_seed(&seed, &src, sizeof src)) return 1;
    FILE* f = fopen(out, "wb");
    if (!f || fwrite(&seed, 1, sizeof seed, f) != sizeof seed) { perror(out); return 1; }
    if (fclose(f) != 0) { perror(out); return 1; }
    printf("испытательное семя для стенда записано в %s (не результат обучения)\n", out);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--fixture")) return fixture(argv[2]);
    if (argc != 3) { fprintf(stderr, "использование: mindseed вход.dat выход.bin\n"); return 2; }
    size_t n; unsigned char* in = slurp(argv[1], &n);
    mind_model_t src, seed;
    if (!mind_model_load(&src, in, (unsigned)n)) { fprintf(stderr, "%s: не файл опыта Разума (размер, версия или контрольная сумма)\n", argv[1]); return 1; }
    printf("опыт: прожито %u с, решений %u, модель предвидения обучена на %u с (опозданий %u), проб кванта %u, удачных %u\n",
           src.lifetime_s, src.decisions, src.net.n, src.net.pos, src.tune_trials, src.tune_wins);
    if (src.lifetime_s < 600 || src.net.n < 600) { fprintf(stderr, "мало опыта (нужно не менее 600 с жизни и обучения): семя не создано\n"); return 1; }
    if (!mind_model_seed(&seed, &src, sizeof src)) { fprintf(stderr, "семя не создано\n"); return 1; }
    unsigned hints = 0;
    for (int i = 0; i < ML_N; i++) {
        printf("рычаг %2d %-44s проб %3u удач %3u лучшее значение: ступень %u%s\n", 13 + i, mind_lever_name(i), src.lv[i].trials, src.lv[i].wins,
               src.lv[i].best, seed.lv[i].wins ? " (в семени)" : "");
        hints += seed.lv[i].wins != 0;
    }
    printf("в семени: веса предвидения (наблюдений %u из %u), подсказок по рычагам %u\n", seed.net.n, src.net.n, hints);
    FILE* f = fopen(argv[2], "wb");
    if (!f || fwrite(&seed, 1, sizeof seed, f) != sizeof seed) { perror(argv[2]); return 1; }
    if (fclose(f) != 0) { perror(argv[2]); return 1; }
    printf("записано %s (%zu байт)\n", argv[2], sizeof seed);
    return 0;
}
