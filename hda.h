#ifndef KIRA_HDA_H
#define KIRA_HDA_H
#include <stdint.h>
/* Звук: Intel HDA по PCI. 0 - контроллер, кодек и путь к выходу найдены. */
int  hda_probe(void);
int  hda_beep(unsigned hz, unsigned ms, uint32_t* dma_bytes);   /* тон: 0 - сыгран; dma_bytes - сколько забрала карта */
void hdacmd_sound(const char* arg);
void hdacmd_beep(const char* arg);
#endif
