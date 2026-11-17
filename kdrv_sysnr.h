#ifndef KDRV_SYSNR_H
#define KDRV_SYSNR_H
/* Номера системных вызовов драйверов Ring 3 (договор - kdrv_format.h). Вынесены отдельно: диспетчер syscall.c (ур.3) знает номера,
   но не форматы пакета драйвера (ур.4). */
enum { KDRV_SYS_INFO = 48,         /* (буфер kdrv_info_t, ёмкость) -> 0: свои устройство и права */
       KDRV_SYS_CFG_READ = 49,     /* (смещение < 256, ширина 1/2/4) -> значение; CAP_PCIDEV */
       KDRV_SYS_CFG_WRITE = 50,    /* (смещение < 256, ширина 1/2/4, значение) -> 0; CAP_PCIDEV; не BAR, не MSI/MSI-X/PCIe, команда - только MEM/IO/MASTER/INTx-off */
       KDRV_SYS_MMIO_MAP = 51,     /* (номер BAR, смещение, длина) -> адрес; CAP_MMIO; страницы устройства без кэша и без исполнения */
       KDRV_SYS_DMA_ALLOC = 52,    /* (байт, предел адреса в битах или 0, kdrv_dma_t*) -> 0; CAP_DMA; освобождается при гибели драйвера */
       KDRV_SYS_IRQ_WAIT = 53,     /* (срок в 10 мс или ~0) -> маска KDRV_EV_* или KE_TIMEOUT; CAP_IRQ */
       KDRV_SYS_NETDEV = 54,       /* (операция KDRV_NET_*, аргумент, флаги) ; CAP_NETDEV */
       KDRV_SYS_PCILIST = 55,      /* (буфер kdrv_pcient_t[], число записей, номер первой) -> записано; ТОЛЬКО ЧТЕНИЕ, право CAP_PCILIST (любая программа, не драйвер) */
       KDRV_SYS_IO = 57,         /* (упаковка kdrv_io_pack, смещение в BAR, значение или буфер) -> значение/0; CAP_MMIO; порты ТОЛЬКО своего I/O BAR, x86 */
       KDRV_SYS_BLKDEV = 58 };     /* (операция KDRV_BLK_*, kdrv_blkreg_t* или 0) ; CAP_BLKDEV; диск драйвера для blk.c (kdrv_blk.h) */
#endif
