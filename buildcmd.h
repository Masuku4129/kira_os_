#ifndef KIRA_BUILDCMD_H
#define KIRA_BUILDCMD_H
/* Консольная команда `build` (задание на сборку, этап D плана сборщика): единственное место ядра, где выдаётся разрешение на
   собранный образ и запускается собранное. Сборку делает отдельный процесс kcc без прав (userland/kcc.c, режим @build);
   ядро только ставит ему задачу, показывает отчёт, принимает решение человека и запускает проверенный образ без прав.
   Вызывается из console.c (ввод человека). Разум (mind_sys.c) получает только build_mind_tick: предложение без сборки. */
#include <stdint.h>
#include "mind.h"

void build_cmd(const char* arg);
/* Раз в секунду из mind_sys_tick: если правило Разума (mind_build_propose) разрешает, печатает в serial строку-предложение и
   запоминает его для `build`. Ничего не собирает и не запускает; не принимает и не выдаёт разрешений. */
void build_mind_tick(const mind_t* m, const mind_sample_t* s, uint32_t now_s);

#endif
