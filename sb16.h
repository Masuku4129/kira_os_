#ifndef KIRA_SB16_H
#define KIRA_SB16_H
#include <stdint.h>
/* Creative Sound Blaster 16 (ISA; в QEMU - `-device sb16`, в Limbo - Sound: sb16). Только x86-64: порты ввода-вывода и
   DMA 8237. 0 - карта найдена и подключена к микшеру; -1 - платформа не сообщает об устройствах ISA или карты нет. */
int sb16_probe(void);

/* Чистые функции (тест на хосте). Регистры смесителя SB16 0x80 (прерывание) и 0x81 (каналы DMA) устройство отдаёт
   само: ровно один установленный бит - выбранное значение, иначе -1.
   0x80: бит0 - IRQ 2 (9), бит1 - 5, бит2 - 7, бит3 - 10.  0x81: биты 0,1,3 - 8-битные каналы 0,1,3; биты 5,6,7 - 16-битные 5,6,7. */
int sb16_irq_from_mixer(uint8_t reg80);
int sb16_dma8_from_mixer(uint8_t reg81);       /* канал 0, 1 или 3 */
int sb16_dma16_from_mixer(uint8_t reg81);      /* канал 5, 6 или 7 */
/* Порты 8237 16-битного канала ch (5..7): адрес, счётчик, страница; 0 для других номеров. */
uint16_t sb16_dma16_port_addr(int ch);
uint16_t sb16_dma16_port_count(int ch);
uint16_t sb16_dma16_port_page(int ch);
#endif
