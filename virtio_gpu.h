#ifndef KIRA_VIRTIO_GPU_H
#define KIRA_VIRTIO_GPU_H
/* Экран virtio-gpu (2D) по PCI. 0 - экран поднят и подключён к fb (fb_attach_linear); <0 - устройства нет/отказ. */
int virtio_gpu_probe(void);
#endif
