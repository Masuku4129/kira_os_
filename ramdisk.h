#ifndef RAMDISK_H
#define RAMDISK_H

#include "blk.h"

void ramdisk_set_boot_info(unsigned long mbi);
blk_dev_t* ramdisk_probe(void);

#endif
