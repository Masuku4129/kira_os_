#ifndef KIRA_LIFE_H
#define KIRA_LIFE_H
#include <stdint.h>

/* Жизнь организма в файловой системе: журнал событий (/var/life.log) и профиль каждой программы (/var/prog/<имя>:
   запуски, аварийные завершения, отказы иммунитета). Всё, что меняет файлы, выполняется из основного потока ядра
   (life_tick): события из других контекстов (завершение процесса с выключенными прерываниями, дисплей) только ставятся
   в очередь, поэтому диск не трогают два потока сразу. */

void life_init(void);                                          /* после монтирования KiraFS */
void life_note(const char* kind, const char* name, int code);  /* из любого контекста; не блокируется; kind: boot|start|exit|killed|denied */
void life_tick(void);                                          /* основной поток: записывает накопленное */
void life_print(void);                                         /* журнал в консоль */
/* Профиль программы: 0 - есть, -1 - нет данных. */
int  life_profile(const char* name, uint32_t* runs, uint32_t* faults, uint32_t* denied, int* last_code);
uint32_t life_dropped(void);                                   /* событий потеряно (переполнение очереди или нет ФС) */

#endif
