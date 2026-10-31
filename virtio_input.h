#ifndef KIRA_VIRTIO_INPUT_H
#define KIRA_VIRTIO_INPUT_H
/* Все устройства virtio-input на PCI (клавиатура, мышь, планшет): события - в общий слой ввода (input.h).
   Возвращает число подключённых устройств. */
int virtio_input_init(void);
#endif
