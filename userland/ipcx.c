/* Проверка границы доменов (caps.h): программа домена media пытается открыть ОБЩУЮ память, созданную программой ipc из домена tools.
   Права CAP_IPC у обеих есть, объект помечен KOPEN_PUBLIC - единственное, что мешает, это домен: ядро обязано ответить KE_PERM.
   Запускается вместе с ipc (команда test ipc). Объект появляется на время обмена ipc, поэтому ждём его и сразу пробуем открыть. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }

int umain(void) {
    i64 r = KE_NOENT;
    for (int i = 0; i < 500 && r == KE_NOENT; i++) {          /* до 5 с: пока ipc не создал объект, ответ KE_NOENT */
        r = k_shm_open("kira.test.shm", 0, 0);
        if (r == KE_NOENT) k_sleep(1);
    }
    i64 p = k_port_open("kira.test", 0);                      /* порт kira.test создаёт ipc без KOPEN_PUBLIC: чужой приватный */
    if (r == KE_PERM && (p == KE_PERM || p == KE_NOENT)) { say("ipcx: foreign-domain open of a public shared-memory object refused (PASS)\n"); return 0; }
    say(r == KE_NOENT ? "ipcx: FAIL, the shared-memory object never appeared\n" : r >= 0 ? "ipcx: FAIL, a foreign domain opened the object\n" : "ipcx: FAIL, wrong error code\n");
    return 1;
}
