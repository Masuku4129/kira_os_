# RTL8139 clean-room driver

The `drivers/rl/rl.c` implementation is a new KIRA driver written against
the public RTL8139 register and DMA programming model. It no longer includes
the former FreeBSD-derived `if_rlreg.h` and does not reuse a BSD/Linux driver
implementation. The small `kdrv_bsd.h` file is KIRA's Ring-3 device ABI
adapter; it supplies bounded MMIO, DMA, and network-ring calls.

Implementation outline:

- PCI identity is checked against Realtek `10ec:8139`; only a memory BAR is
  mapped, and PCI memory/bus-master mode is enabled explicitly.
- The driver requests a 64 KiB RX ring and four 2 KiB TX buffers through
  `kd_dma_alloc(..., 32)`. Every physical range is checked against the
  RTL8139 32-bit DMA limit.
- Reset, MAC read, receive/transmit configuration, station/multicast filters,
  descriptor programming, RX wrap handling, frame-length/FCS checks, TX
  completion/error handling, and link polling are implemented directly.
- The driver polls rather than requesting an interrupt capability.
- Received frames are copied into the kernel-owned netdev RX ring; transmit
  frames are dequeued from the kernel-owned TX ring.

**DMA safety requirement:** KDRV grants `CAP_DMA` only when the device is
isolated by the platform IOMMU policy. The driver deliberately has no
unisolated physical-address fallback. On a machine without an active isolation
domain, the driver refuses to start rather than exposing arbitrary memory.

The recipe `drivers/rl/drvget.rec` includes both `rl.c` and
`kira_rtl8139_core.h`; their SHA-256 hashes are pinned in the recipe and the
recipe hash is pinned in `drivers/index.txt`.
