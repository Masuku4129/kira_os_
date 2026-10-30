#ifndef EXPERIENCE_H
#define EXPERIENCE_H
#include <stdint.h>

/* Связка «организм <-> диск»: восстановление опыта при загрузке и периодическое сохранение. */
void experience_init(void);
void experience_attach(void);                    /* хранилище опыта - на текущий системный диск */
void experience_tick(uint64_t session_seconds);   /* раз в секунду */
int  experience_save_now(void);                   /* 0 - записано */
void experience_print(void);
int  experience_enabled(void);

#endif
