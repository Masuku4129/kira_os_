/* Sound Blaster 16: разбор регистров смесителя 0x80/0x81, которые карта отдаёт сама, и порты 8237 для 16-битных каналов. */
#include <stdio.h>
#include "../sb16.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* QEMU и обычная карта: IRQ 5, 8-битный канал 1, 16-битный 5 -> 0x02 и 0x22 */
    CHECK(sb16_irq_from_mixer(0x02) == 5 && sb16_dma8_from_mixer(0x22) == 1 && sb16_dma16_from_mixer(0x22) == 5);
    CHECK(sb16_irq_from_mixer(0x01) == 2 && sb16_irq_from_mixer(0x04) == 7 && sb16_irq_from_mixer(0x08) == 10);
    CHECK(sb16_dma8_from_mixer(0x01) == 0 && sb16_dma8_from_mixer(0x08) == 3);
    CHECK(sb16_dma16_from_mixer(0x40) == 6 && sb16_dma16_from_mixer(0x80) == 7);
    /* ничего или несколько - устройство не сказало однозначно */
    CHECK(sb16_irq_from_mixer(0x00) == -1 && sb16_irq_from_mixer(0x03) == -1 && sb16_irq_from_mixer(0x0F) == -1);
    CHECK(sb16_dma8_from_mixer(0x00) == -1 && sb16_dma8_from_mixer(0x03) == -1 && sb16_dma8_from_mixer(0x20) == -1);
    CHECK(sb16_dma16_from_mixer(0x00) == -1 && sb16_dma16_from_mixer(0x60) == -1 && sb16_dma16_from_mixer(0x02) == -1);
    /* старшие биты чужих полей не мешают */
    CHECK(sb16_irq_from_mixer(0xF2) == 5 && sb16_dma8_from_mixer(0xE2) == 1 && sb16_dma16_from_mixer(0x2F) == 5);
    /* 8237: ведомый контроллер 0xC0.. (шаг 2 на регистр, 4 на канал), страницы 5 -> 0x8B, 6 -> 0x89, 7 -> 0x8A */
    CHECK(sb16_dma16_port_addr(5) == 0xC4 && sb16_dma16_port_count(5) == 0xC6 && sb16_dma16_port_page(5) == 0x8B);
    CHECK(sb16_dma16_port_addr(6) == 0xC8 && sb16_dma16_port_count(6) == 0xCA && sb16_dma16_port_page(6) == 0x89);
    CHECK(sb16_dma16_port_addr(7) == 0xCC && sb16_dma16_port_count(7) == 0xCE && sb16_dma16_port_page(7) == 0x8A);
    CHECK(sb16_dma16_port_addr(4) == 0 && sb16_dma16_port_addr(1) == 0 && sb16_dma16_port_page(8) == 0);
    printf(fails ? "SB16 TESTS FAILED\n" : "sb16: mixer register decoding (IRQ, 8/16-bit DMA, ambiguous/empty values rejected) and 8237 16-bit channel ports passed\n");
    return fails != 0;
}
