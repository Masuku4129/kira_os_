#ifndef KIRA_HDA_H
#define KIRA_HDA_H
#include <stdint.h>
/* Звук: Intel HDA по PCI. 0 - контроллер, кодек и путь к выходу найдены. */
int  hda_probe(void);
int  hda_beep(unsigned hz, unsigned ms, uint32_t* dma_bytes);   /* тон: 0 - сыгран; dma_bytes - сколько забрала карта */
void hdacmd_sound(const char* arg);
/* Для микшера (audio.c): кольцо вывода, по которому карта идёт по кругу. */
int      hda_ring_start(void);
uint32_t hda_ring_pos(void);          /* байт от начала кольца, где сейчас карта */
int16_t* hda_ring_buf(void);
uint32_t hda_ring_bytes(void);
void hdacmd_beep(const char* arg);
#endif
