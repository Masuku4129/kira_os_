# RTL8139 clean-room implementation provenance

- Device family: Realtek RTL8139C / RTL8139CL (PCI vendor/device `10ec:8139`)
- Specification: Realtek Semiconductor Corp., *RTL8139C(L) 3.3V Single-Chip Fast Ethernet Controller with Power Management*, Rev. 1.6, 29 December 2005, Track ID JATR-1076-21.
- Public specification mirror: https://people.freebsd.org/~wpaul/RealTek/spec-8139c%28160%29.pdf
- Relevant sections: 4.1 package/version identification; 6.1 RX packet header; 6.2 TSD0–TSD3; 6.4 command register; 6.5 IMR; 6.6 ISR; 6.7 TCR; 6.8 RCR; 9.1 transmit operation; 9.2 receive operation.
- Implementation: `drivers/net/rtl8139.c` and `drivers/net/rtl8139.h`.
- Host validation: `tests/rtl8139_test.c`, executed by `tests/run_host_tests.sh`.

## Provenance boundary

The new `drivers/net/rtl8139.c` implementation is written against the
published register/descriptor contract and KIRA's own platform callbacks.
No FreeBSD/Linux source file, function body, macro block, or compatibility
header is used as an implementation input. The legacy
`drivers/rl/rl.c` and `if_rlreg.h` remain a separate BSD-3-Clause-derived
KDRV path; they have **not** been replaced or removed by this clean-room
module. The project must not claim the production RTL8139 recipe is clean-room
until that legacy path is removed from the shipped catalogue and the new
driver is integrated into the supported network-device path.

## Current implementation contract

- PCI identity must be exactly 10ec:8139; the caller supplies a validated I/O
  BAR base and region length.
- DMA allocations must be 256-byte aligned and wholly addressable by the
  controller's 32-bit DMA registers. Allocation failure or invalid physical
  range stops the controller and frees every prior allocation.
- The platform supplies explicit cache invalidate/clean and DMA read/write
  barriers. RX invalidates both ring header and payload spans before CPU reads;
  TX cleans the payload and issues a write barrier before ownership is handed
  to the device.
- The driver is polling-only and keeps IMR disabled; it does not claim an IRQ
  path. RX validates status and packet length, strips FCS, wraps to the ring
  start (RCR.WRAP=0), and resets ring ownership on impossible length metadata.
- TX waits for the hardware OWN completion bit and has a one-second monotonic
  deadline. Timeout resets the controller before any DMA buffer is made
  reusable. If reset cannot be confirmed, the driver disables itself and
  deliberately retains DMA allocations; `shutdown()` can be retried.
- TX slot reuse is gated by the hardware OWN completion bit; TOK distinguishes
  successful completion. Link state is read from the Basic Mode Status
  Register.

## Evidence and remaining work

The host model exercises PCI rejection, reset timeout, partial DMA allocation
cleanup, 32-bit DMA bounds, TX padding/completion/error/timeout, reset-before-free,
RX validation,
ring wrap, FAE rejection, malformed length recovery, cache/barrier callbacks,
link state and shutdown. It is a deterministic host model, not QEMU device
emulation and not physical-card evidence.

Still required before replacing the legacy recipe:
1. Run the host test and x86/ARM64 warning-as-error builds on the exact commit.
2. Integrate PCI discovery, I/O BAR ownership, bus-master gating and the
   network-device registration path.
3. Add a QEMU RTL8139 scenario proving real guest packet RX/TX and fault cases.
4. Review non-coherent DMA/cache maintenance on ARM64 and physical hardware.
5. Remove the BSD-derived recipe/source only after the clean-room path passes
   the same functional matrix; retain required BSD notices until its code is
   no longer distributed.
