#ifndef KIRA_RCON_H
#define KIRA_RCON_H
#include <stdint.h>

/* Удалённая консоль по TCP: те же команды, что в терминале, и тот же вывод (журнал ядра).
   Выключена по умолчанию. Включается командой «rcon on» только после «rcon passwd ПАРОЛЬ».
   Протокол - строки текста: сервер шлёт «KIRA-RCON 1», клиент - «AUTH пароль»; ответ «OK» или «ERR».
   Потом каждая строка клиента - команда консоли, а всё, что печатает ядро, идёт клиенту потоком.
   Служебные строки клиента: PING (ответ PONG), QUIT. Пароль идёт открытым текстом: только доверенная сеть или ВМ. */

#define RCON_PORT_DEFAULT 7077
#define RCON_KEY_LEN 48            /* соль 16 + SHA-256 с растяжкой 32 */

/* Чистые функции (проверяются на хосте) */
void rcon_make_key(const uint8_t salt[16], const char* pass, uint8_t key[RCON_KEY_LEN]);
int  rcon_check_pass(const uint8_t key[RCON_KEY_LEN], const char* pass);      /* 1 - верно; сравнение за постоянное время */
/* Разбор строки «AUTH пароль»: указатель на пароль или 0, если строка не та. Конец строки (\r, \n) убирается. */
const char* rcon_parse_auth(char* line);

void rcon_init(void);              /* поток удалённой консоли (из console_init); ждёт файловую систему и сеть */
void rconcmd(const char* arg);     /* команда «rcon»: passwd, on, off, status */
#endif
