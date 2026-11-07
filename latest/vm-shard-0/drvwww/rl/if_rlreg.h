/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Описание регистров RealTek RTL8139 для порта rl(4) на драйвер Ring 3 Киры.
 * Основано на FreeBSD sys/dev/rl/if_rlreg.h (rl(4)): имена регистров и битов оставлены прежними, чтобы порт читался рядом с оригиналом.
 * Файл написан заново по этому описанию, не скопирован построчно.
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
#ifndef _IF_RLREG_H_
#define _IF_RLREG_H_

#define RL_VENDORID_REALTEK	0x10EC
#define RL_DEVICEID_8139	0x8139

/* Смещения регистров (доступ через MMIO BAR1: те же, что в окне портов BAR0). */
#define RL_IDR0		0x0000		/* ID register 0 (station addr) */
#define RL_IDR4		0x0004
#define RL_MAR0		0x0008		/* Multicast hash table (8 байт) */
#define RL_TXSTAT0	0x0010		/* status of TX descriptor 0 */
#define RL_TXADDR0	0x0020		/* address of TX descriptor 0 */
#define RL_RXADDR	0x0030		/* RX ring start address */
#define RL_COMMAND	0x0037		/* command register */
#define RL_CURRXADDR	0x0038		/* current address of packet read (CAPR) */
#define RL_CURRXBUF	0x003A		/* current RX buffer address (CBR) */
#define RL_IMR		0x003C		/* interrupt mask register */
#define RL_ISR		0x003E		/* interrupt status register */
#define RL_TXCFG	0x0040		/* transmit config */
#define RL_RXCFG	0x0044		/* receive config */
#define RL_TIMERCNT	0x0048		/* timer count register */
#define RL_MISSEDPKT	0x004C		/* missed packet counter */
#define RL_EECMD	0x0050		/* EEPROM command register */
#define RL_CFG0		0x0051
#define RL_CFG1		0x0052
#define RL_MEDIASTAT	0x0058		/* media status register */
#define RL_BMCR		0x0062		/* PHY basic mode control */
#define RL_BMSR		0x0064		/* PHY basic mode status */

/* RL_COMMAND */
#define RL_CMD_EMPTY_RXBUF	0x0001
#define RL_CMD_TX_ENB		0x0004
#define RL_CMD_RX_ENB		0x0008
#define RL_CMD_RESET		0x0010

/* RL_ISR / RL_IMR */
#define RL_ISR_RX_OK		0x0001
#define RL_ISR_RX_ERR		0x0002
#define RL_ISR_TX_OK		0x0004
#define RL_ISR_TX_ERR		0x0008
#define RL_ISR_RX_OVERRUN	0x0010
#define RL_ISR_PKT_UNDERRUN	0x0020
#define RL_ISR_FIFO_OFLOW	0x0040
#define RL_ISR_PCS_TIMEOUT	0x4000
#define RL_ISR_SYSTEM_ERR	0x8000

/* RL_TXCFG */
#define RL_TXCFG_IFG		0x03000000	/* interframe gap */
#define RL_TXDMA_2048BYTES	0x00000700
#define RL_TXCFG_CONFIG		(RL_TXCFG_IFG | RL_TXDMA_2048BYTES)

/* RL_RXCFG */
#define RL_RXCFG_RX_ALLPHYS	0x00000001	/* accept all nodes */
#define RL_RXCFG_RX_INDIV	0x00000002	/* match filter */
#define RL_RXCFG_RX_MULTI	0x00000004	/* accept all multicast */
#define RL_RXCFG_RX_BROAD	0x00000008	/* accept all broadcast */
#define RL_RXCFG_RX_RUNT	0x00000010
#define RL_RXCFG_RX_ERRPKT	0x00000020
#define RL_RXFIFO_NOTHRESH	0x0000E000
#define RL_RXDMA_UNLIMITED	0x00000700
#define RL_RXBUF_8		0x00000000
#define RL_RXBUF_16		0x00000800
#define RL_RXBUF_32		0x00001000
#define RL_RXBUF_64		0x00001800
#define RL_RX_BUF_SZ		RL_RXBUF_64
#define RL_RXBUFLEN		(1 << ((RL_RX_BUF_SZ >> 11) + 13))
#define RL_RXCFG_CONFIG		(RL_RXFIFO_NOTHRESH | RL_RXDMA_UNLIMITED | RL_RX_BUF_SZ)

/* RL_EECMD */
#define RL_EE_DATA_WRITE	0x02		/* data out of the EEPROM chip */
#define RL_EE_DATA_READ		0x01		/* data into the EEPROM chip */
#define RL_EE_CLK		0x04
#define RL_EE_SEL		0x08
#define RL_EEMODE_OFF		0x00
#define RL_EEMODE_PROGRAM	0x80
#define RL_EEMODE_WRITECFG	0xC0
#define RL_EECMD_READ_6BIT	0x0180		/* 93C46: старт 1, код чтения 10, 6 бит адреса */
#define RL_EE_ID		0x00
#define RL_EE_EADDR		0x07		/* слово 7: начало MAC-адреса (3 слова) */

/* RL_TXSTAT */
#define RL_TXSTAT_LENMASK	0x00001FFF
#define RL_TXSTAT_OWN		0x00002000
#define RL_TXSTAT_TX_UNDERRUN	0x00004000
#define RL_TXSTAT_TX_OK		0x00008000
#define RL_TXSTAT_TXABRT	0x40000000
#define RL_TX_EARLYTHRESH	((256 / 32) << 16)
#define RL_TX_THRESH_MAX	((1536 / 32) << 16)
#define RL_TX_LIST_CNT		4

/* заголовок принятого кадра в кольце: 16 бит состояние, 16 бит длина (с CRC) */
#define RL_RXSTAT_RXOK		0x00000001
#define RL_RXSTAT_UNFINISHED	0x0000FFF0	/* чип ещё копирует кадр */
#define RL_RX_HDR		4

/* RL_BMSR / RL_MEDIASTAT */
#define RL_BMSR_LINK		0x0004
#define RL_MEDIASTAT_LINKFAIL	0x0004		/* 1 - канала нет */

#define RL_MIN_FRAMELEN		60		/* ETHER_MIN_LEN - ETHER_CRC_LEN: короче кадр дополняется нулями */
#define RL_RXRING_BYTES		(RL_RXBUFLEN + 16 + 1536)

#endif
