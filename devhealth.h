#ifndef DEVHEALTH_H
#define DEVHEALTH_H
#include <stdint.h>

/* Здоровье устройств - для Разума. Драйверы сообщают об ошибках обмена (диски, флешки, USB-устройства) и о рефлексе
   ввода (устройство печатает быстрее человека и задержано); Разум раз в секунду забирает накопленное и решает. */
void devhealth_error(const char* dev);                                   /* из любого потока */
void devhealth_take(uint32_t* errors, char name[16]);                    /* накопленное с прошлого раза */
void devhealth_hid_suspect(const char* dev, uint32_t keys_per_s);
void devhealth_hid_take(uint32_t* n, uint32_t* rate, char name[16]);

#endif
