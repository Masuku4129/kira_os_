#ifndef KIRA_FB_ARM_H
#define KIRA_FB_ARM_H
#include <stdint.h>
/* Экран ARM64 без virtio-gpu (ramfb, simple-framebuffer); platform_fb_probe - в platform.h. */
void arm_fb_init(const void* fdt);
void arm_fb_reserve(const void* fdt, void (*reserve)(uint64_t lo, uint64_t hi));
int  arm_mem_in_ram(uint64_t a, uint64_t len);       /* main.c: участок целиком в ОЗУ (кэшируемая карта) */
#endif
