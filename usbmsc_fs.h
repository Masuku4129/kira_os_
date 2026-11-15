#ifndef USBMSC_FS_H
#define USBMSC_FS_H
#include "usb.h"
/* Операции томов для накопителей USB: msc_set_fs_ops(&usbmsc_fs_ops) (wire_init; хост-тесты - в main). */
extern const msc_fs_ops_t usbmsc_fs_ops;
#endif
