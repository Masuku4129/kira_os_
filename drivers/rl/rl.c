/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Драйвер RealTek RTL8139 (10ec:8139) для Киры, Ring 3 / EL0: порт rl(4) из FreeBSD (sys/dev/rl/if_rl.c) на слой kdrv_bsd.h.
 * Порядок работы взят из rl(4): rl_reset, чтение MAC из 93C46 побитовым обменом (rl_eeprom_getword), rl_init (кольцо приёма 64 КБ + 16,
 * четыре передающих дескриптора с копированием кадра в собственный буфер, дополнение до 60 байт, порог раннего старта), rl_rxeof
 * (заголовок состояние/длина, обрезка CRC, перенос через конец кольца, запись CAPR = cur_rx - 16), rl_txeof (TOK/TUN/TABT).
 * Отличия от оригинала (из-за Киры): вместо mbuf и ifnet - кольца netdev_register (ifnet-лайт из kdrv_bsd.h); вместо MII - чтение BMSR/MSR;
 * прерываний INTx ядро драйверам не отдаёт (только MSI), у RTL8139 MSI нет: IMR = 0, приём и завершение передачи опрашиваются каждые
 * 10 мс, передача от ядра будит драйвер сразу (kd_wait). Код написан заново по логике оригинала, не скопирован построчно.
 *
 * Copyright (c) 1997, 1998
 *	Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "kdrv_bsd.h"
#include "if_rlreg.h"
#include "../rtl8139/kira_rtl8139_core.h"

struct rl_softc {
	struct ifnet ifnet;
	bus_space_handle_t bsh;
	struct kd_dmabuf rxbuf;		/* кольцо приёма: RL_RXBUFLEN + 16 + 1536 */
	struct kd_dmabuf txbuf;		/* RL_TX_LIST_CNT буферов по 2048 байт */
	u32 cur_rx;			/* смещение следующего кадра в кольце, [0, RL_RXBUFLEN) */
	u32 cur_tx, dirty_tx, inflight;	/* передающие дескрипторы: куда писать, какой ждём, сколько занято */
	u32 txthresh;			/* порог раннего старта передачи, в единицах 32 байт */
	u8 eaddr[ETHER_ADDR_LEN];
	int link;
	int saw_rx, saw_tx;
};

#define CSR_WRITE_1(sc, reg, val)	bus_space_write_1((sc)->bsh, reg, val)
#define CSR_WRITE_2(sc, reg, val)	bus_space_write_2((sc)->bsh, reg, val)
#define CSR_WRITE_4(sc, reg, val)	bus_space_write_4((sc)->bsh, reg, val)
#define CSR_READ_1(sc, reg)		bus_space_read_1((sc)->bsh, reg)
#define CSR_READ_2(sc, reg)		bus_space_read_2((sc)->bsh, reg)
#define CSR_READ_4(sc, reg)		bus_space_read_4((sc)->bsh, reg)

static struct rl_softc rl_sc;

/* ---- 93C46: побитовый обмен через RL_EECMD (rl_eeprom_putbyte / rl_eeprom_getword) ---- */
static void rl_eeprom_putbyte(struct rl_softc *sc, int addr)
{
	int d, i;

	d = addr | RL_EECMD_READ_6BIT;
	for (i = 0x100; i; i >>= 1) {
		int bit = (d & i) ? RL_EE_DATA_WRITE : 0;
		CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL | bit);
		DELAY(100);
		CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL | bit | RL_EE_CLK);
		DELAY(150);
		CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL | bit);
		DELAY(100);
	}
}

static u16 rl_eeprom_getword(struct rl_softc *sc, int addr)
{
	u16 word = 0;
	int i;

	CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL);
	DELAY(100);
	rl_eeprom_putbyte(sc, addr);
	for (i = 0x8000; i; i >>= 1) {
		CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL | RL_EE_CLK);
		DELAY(100);
		if (CSR_READ_1(sc, RL_EECMD) & RL_EE_DATA_READ)
			word |= (u16)i;
		CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_PROGRAM | RL_EE_SEL);
		DELAY(100);
	}
	CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_OFF);
	return word;
}

static void rl_reset(struct rl_softc *sc)
{
	int i;

	CSR_WRITE_1(sc, RL_COMMAND, RL_CMD_RESET);
	for (i = 0; i < 1000; i++) {
		DELAY(10);
		if (!(CSR_READ_1(sc, RL_COMMAND) & RL_CMD_RESET))
			break;
	}
	if (i == 1000)
		kd_puts("rl: reset never completed!\n");
}

/* MAC из EEPROM (слова RL_EE_EADDR..+2); если там мусор - из IDR0..5, которые чип загружает сам. 1 - из EEPROM. */
static int rl_read_mac(struct rl_softc *sc)
{
	u8 eep[ETHER_ADDR_LEN], idr[ETHER_ADDR_LEN];
	int i, ok;
	u16 w = rl_eeprom_getword(sc, RL_EE_ID);

	for (i = 0; i < 3; i++) {
		u16 x = rl_eeprom_getword(sc, RL_EE_EADDR + i);
		eep[i * 2] = (u8)(x & 0xff);
		eep[i * 2 + 1] = (u8)(x >> 8);
	}
	for (i = 0; i < ETHER_ADDR_LEN; i++)
		idr[i] = CSR_READ_1(sc, RL_IDR0 + i);
	ok = !(eep[0] & 1) && (eep[0] | eep[1] | eep[2] | eep[3] | eep[4] | eep[5]) &&
	    (eep[0] & eep[1] & eep[2] & eep[3] & eep[4] & eep[5]) != 0xff;
	kd_puts("rl: EEPROM id "); kd_puthex(w, 4); kd_puts(", EEPROM MAC "); kd_putmac(eep); kd_puts(", IDR "); kd_putmac(idr); kd_puts("\n");
	kd_memcpy(sc->eaddr, ok ? eep : idr, ETHER_ADDR_LEN);
	return ok;
}

static void rl_stop(struct rl_softc *sc)
{
	CSR_WRITE_1(sc, RL_COMMAND, 0x00);
	CSR_WRITE_2(sc, RL_IMR, 0x0000);
	sc->inflight = sc->cur_tx = sc->dirty_tx = 0;
}

static void rl_init(struct rl_softc *sc)
{
	int i;

	rl_stop(sc);
	rl_reset(sc);

	/* адрес станции */
	CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_WRITECFG);
	CSR_WRITE_4(sc, RL_IDR0, sc->eaddr[0] | (sc->eaddr[1] << 8) | (sc->eaddr[2] << 16) | ((u32)sc->eaddr[3] << 24));
	CSR_WRITE_4(sc, RL_IDR4, sc->eaddr[4] | (sc->eaddr[5] << 8));
	CSR_WRITE_1(sc, RL_EECMD, RL_EEMODE_OFF);

	/* кольцо приёма; CAPR после сброса = 0xFFF0 (cur_rx = 0) */
	kd_memset(sc->rxbuf.va, 0, 64);
	sc->cur_rx = 0;
	CSR_WRITE_4(sc, RL_RXADDR, (u32)sc->rxbuf.pa);
	CSR_WRITE_2(sc, RL_CURRXADDR, 0xFFF0);

	/* буферы передачи */
	for (i = 0; i < RL_TX_LIST_CNT; i++)
		CSR_WRITE_4(sc, RL_TXADDR0 + i * 4, (u32)(sc->txbuf.pa + (u64)i * 2048));

	CSR_WRITE_1(sc, RL_COMMAND, RL_CMD_TX_ENB | RL_CMD_RX_ENB);
	CSR_WRITE_4(sc, RL_TXCFG, RL_TXCFG_CONFIG);
	CSR_WRITE_4(sc, RL_RXCFG, RL_RXCFG_CONFIG | RL_RXCFG_RX_INDIV | RL_RXCFG_RX_BROAD | RL_RXCFG_RX_MULTI);
	CSR_WRITE_4(sc, RL_MISSEDPKT, 0);

	/* групповые адреса: принимаем все (ядро не передаёт карте списка - netdev.mcast_add = 0) */
	CSR_WRITE_4(sc, RL_MAR0, 0xFFFFFFFF);
	CSR_WRITE_4(sc, RL_MAR0 + 4, 0xFFFFFFFF);

	CSR_WRITE_2(sc, RL_ISR, 0xFFFF);
	CSR_WRITE_2(sc, RL_IMR, 0x0000);	/* прерывания INTx драйверу не отдаются: опрос */
	sc->txthresh = RL_TX_EARLYTHRESH >> 16;
}

static int rl_link_up(struct rl_softc *sc)
{
	return (CSR_READ_2(sc, RL_BMSR) & RL_BMSR_LINK) || !(CSR_READ_1(sc, RL_MEDIASTAT) & RL_MEDIASTAT_LINKFAIL);
}

static void rl_rxeof(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->ifnet;
	u8 *rx = sc->rxbuf.va;
	u8 frame[ETHER_MAX_LEN + 8];
	u32 cur_rx = sc->cur_rx;
	int budget = 64;

	while (budget-- > 0 && !(CSR_READ_1(sc, RL_COMMAND) & RL_CMD_EMPTY_RXBUF)) {
		kira_rl_rx_record_t record;
		u8 header[4];
		u32 total_len, len, pos;

		if (le32toh(*(volatile u32 *)(rx + cur_rx)) >> 16 == RL_RXSTAT_UNFINISHED)
			break; /* чип ещё копирует кадр в ОЗУ */
		header[0] = ((volatile u8 *)rx)[cur_rx + 0];
		header[1] = ((volatile u8 *)rx)[cur_rx + 1];
		header[2] = ((volatile u8 *)rx)[cur_rx + 2];
		header[3] = ((volatile u8 *)rx)[cur_rx + 3];
		if (kira_rl_rx_decode(header, &record) != 0 ||
		    !kira_rl_rx_status_ok(record.status)) {
			ifp->if_ierrors++;
			rl_init(sc);			/* как rl(4): неверная голова кадра - перезапуск */
			return;
		}
		total_len = record.wire_length;
		len = record.payload_length;	/* чип присылает CRC с каждым кадром */
		pos = (cur_rx + RL_RX_HDR) % RL_RXBUFLEN;
		if (kira_rl_ring_copy_out(rx, RL_RXBUFLEN, pos, frame, len) != 0) {
			ifp->if_ierrors++;
			rl_init(sc);
			return;
		}
		if (!sc->saw_rx) {
			sc->saw_rx = 1;
			kd_puts("rl: first frame received, "); kd_putu(len); kd_puts(" bytes\n");
		}
		(void)if_input(ifp, frame, len);
		cur_rx = (cur_rx + RL_RX_HDR + total_len + 3) & ~3u;
		cur_rx %= RL_RXBUFLEN;
		CSR_WRITE_2(sc, RL_CURRXADDR, (u16)(cur_rx - 16));
	}
	sc->cur_rx = cur_rx;
}

static void rl_txeof(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->ifnet;

	while (sc->inflight) {
		u32 st = CSR_READ_4(sc, RL_TXSTAT0 + sc->dirty_tx * 4);

		if (!(st & (RL_TXSTAT_TX_OK | RL_TXSTAT_TX_UNDERRUN | RL_TXSTAT_TXABRT)))
			break;
		if (st & RL_TXSTAT_TX_OK) {
			ifp->if_opackets++;
			if (!sc->saw_tx) {
				sc->saw_tx = 1;
				kd_puts("rl: first frame transmitted\n");
			}
		} else {
			ifp->if_oerrors++;
			if ((st & RL_TXSTAT_TX_UNDERRUN) && sc->txthresh < (RL_TX_THRESH_MAX >> 16))
				sc->txthresh++;		/* повышаем порог раннего старта, как rl_txeof */
			if (st & RL_TXSTAT_TXABRT)
				CSR_WRITE_4(sc, RL_TXCFG, RL_TXCFG_CONFIG);
		}
		sc->dirty_tx = (sc->dirty_tx + 1) % RL_TX_LIST_CNT;
		sc->inflight--;
	}
}

static void rl_start(struct rl_softc *sc)
{
	struct ifnet *ifp = &sc->ifnet;
	u8 tmp[ETHER_MAX_LEN + 8];

	while (sc->inflight < RL_TX_LIST_CNT && if_tx_pending(ifp)) {
		u32 len = if_dequeue(ifp, tmp, sizeof tmp);
		u32 payload_len = len;
		u16 hardware_len;
		u8 *dst;

		if (!len || kira_rl_tx_length(len, &hardware_len) != 0) {
			ifp->if_oerrors++;
			continue;
		}
		len = hardware_len;
		dst = sc->txbuf.va + sc->cur_tx * 2048;
		kd_memcpy(dst, tmp, payload_len);
		if (len > payload_len)
			kd_memset(dst + payload_len, 0, len - payload_len);
		bus_space_barrier();
		CSR_WRITE_4(sc, RL_TXADDR0 + sc->cur_tx * 4, (u32)(sc->txbuf.pa + (u64)sc->cur_tx * 2048));
		CSR_WRITE_4(sc, RL_TXSTAT0 + sc->cur_tx * 4, (sc->txthresh << 16) | (len & RL_TXSTAT_LENMASK));
		sc->cur_tx = (sc->cur_tx + 1) % RL_TX_LIST_CNT;
		sc->inflight++;
	}
}

int main(void)
{
	struct rl_softc *sc = &rl_sc;
	kdrv_info_t in;
	i64 err = 0, rc;
	u32 bar;
	int i, eep, ticks = 0;

	if (kdrv_info(&in) != 0) {
		kd_puts("rl: no device bound\n");
		return 1;
	}
	if (in.vendor != RL_VENDORID_REALTEK || in.device != RL_DEVICEID_8139) {
		kd_puts("rl: not an RTL8139\n");
		return 1;
	}
	for (bar = 0; bar < 6; bar++)		/* регистры доступны и через порты (BAR0), и через память (BAR1): берём память */
		if (in.bar[bar].size && !(in.bar[bar].flags & KDRV_BAR_IO))
			break;
	if (bar == 6) {
		kd_puts("rl: no memory BAR\n");
		return 1;
	}
	if (in.bar[bar].size < 0x100) {
		kd_puts("rl: MMIO BAR is smaller than the RTL8139 register window\\n");
		return 1;
	}
	sc->bsh = bus_space_map(bar, in.bar[bar].size, &err);
	if (!sc->bsh) {
		kd_puts("rl: cannot map BAR, rc="); kd_putu((u64)(-err)); kd_puts("\n");
		return 1;
	}
	/* INTx не используется: выключить его у устройства (бит 10 команды PCI), чтобы линия не висела */
	{
		i64 cmd = kdrv_cfg_read(0x04, 2);
		if (cmd >= 0 && kdrv_cfg_write(0x04, 2, (u32)cmd | 0x400) != 0)
			kd_puts("rl: cannot disable INTx\n");
	}
	rc = kd_dma_alloc(&sc->rxbuf, RL_RXRING_BYTES, 32);	/* RBSTART - 32 бита */
	if (!rc)
		rc = kd_dma_alloc(&sc->txbuf, RL_TX_LIST_CNT * 2048, 32);
	if (rc) {
		kd_puts("rl: no DMA memory, rc="); kd_putu((u64)(-rc)); kd_puts("\n");
		return 1;
	}
	rl_reset(sc);
	eep = rl_read_mac(sc);
	rl_init(sc);
	kd_puts("rl: RTL8139 BAR"); kd_putu(bar); kd_puts(", MAC "); kd_putmac(sc->eaddr);
	kd_puts(eep ? " (EEPROM)" : " (IDR)");
	kd_puts(", rx ring "); kd_putu(RL_RXBUFLEN); kd_puts(" bytes at 0x"); kd_puthex(sc->rxbuf.pa, 8); kd_puts("\n");

	kd_memcpy(sc->ifnet.if_lladdr, sc->eaddr, ETHER_ADDR_LEN);
	sc->ifnet.if_softc = sc;
	if (if_attach(&sc->ifnet) != 0) {
		kd_puts("rl: netdev_register refused\n");
		return 1;
	}
	sc->link = rl_link_up(sc);
	if_link_state_change(&sc->ifnet, sc->link);
	kd_puts("rl: network interface registered, link "); kd_puts(sc->link ? "up" : "down");
	kd_puts(" (BMSR 0x"); kd_puthex(CSR_READ_2(sc, RL_BMSR), 4); kd_puts(", MSR 0x"); kd_puthex(CSR_READ_1(sc, RL_MEDIASTAT), 2); kd_puts(")\n");

	for (i = 0;; i++) {
		rc = kd_wait(1);		/* до 10 мс; передача от ядра прерывает ожидание сразу */
		if (rc == KE_INTR)
			break;
		if (rc < 0 && rc != KE_TIMEOUT)
			k_sleep(1);	/* иной отказ ожидания не должен превращаться в быстрый цикл */
		rl_start(sc);
		rl_txeof(sc);
		rl_rxeof(sc);
		rl_start(sc);
		if (++ticks >= 50) {		/* состояние канала - примерно раз в полсекунды */
			int up = rl_link_up(sc);
			ticks = 0;
			if (up != sc->link) {
				sc->link = up;
				if_link_state_change(&sc->ifnet, up);
				kd_puts("rl: link "); kd_puts(up ? "up\n" : "down\n");
			}
		}
	}
	rl_stop(sc);
	kd_puts("rl: stopped\n");
	return 0;
}
