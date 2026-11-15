/* no licence information */
    void *tx_buffer[KIRA_RTL8139_TX_SLOTS];
    uint64_t tx_physical[KIRA_RTL8139_TX_SLOTS];
    uint16_t rx_offset;
    uint8_t tx_next;                     /* следующий дескриптор передачи: чип обслуживает их строго по кругу */
    uint8_t tx_busy[KIRA_RTL8139_TX_SLOTS];
    uint8_t initialized;
};

int kira_rtl8139_init(struct kira_rtl8139 *device, uint16_t io_base,
                      const struct kira_rtl8139_platform *platform);
int kira_rtl8139_poll(struct kira_rtl8139 *device);
int kira_rtl8139_send(struct kira_rtl8139 *device,
                      const void *frame, size_t length);
int kira_rtl8139_link_up(struct kira_rtl8139 *device);   /* 1 - канал есть */
void kira_rtl8139_shutdown(struct kira_rtl8139 *device);

#endif
