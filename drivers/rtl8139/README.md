# Native RTL8139 KDRV driver

`rtl8139.c` is a KIRA-native Ring 3 driver implemented against the public
RTL8139 register and DMA programming model. It uses the KDRV API directly;
it does not include the BSD compatibility shim or the historical
`drivers/rl/rl.c` source.

The driver:
- discovers the bound 10ec:8139 device and selects a memory BAR;
- enables PCI memory decoding and bus mastering while disabling legacy INTx;
- allocates and validates 32-bit DMA ranges for the 64 KiB receive ring and
  four transmit buffers;
- resets and configures the controller, reads the station address, and
  registers a KDRV network device;
- bounds RX processing, validates record status/length, handles ring wrap,
  removes the FCS, and publishes frames into the kernel receive ring;
- tracks four hardware TX descriptors and reaps success/error completions;
- polls link state and uses bounded waits so a missing MSI/INTx route does not
  stall the driver.

Required package capabilities: `mmio, dma, pcidev, netdev`. The KDRV DMA
allocator must grant a 32-bit device-visible address range; failure is closed.
The driver source and datapath header are installed into the KCC include tree
by the image build. The legacy BSD-derived port remains separate and is not
used by this native implementation.
