/** @file MUSB register layout used by the bounded PIO host implementation.
  Numeric register definitions verified against Motorola kernel-sprd commit
  261390cdfff66efef9082bd70c1af0abd21d77f2, drivers/usb/musb/musb_regs.h.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef MOTO_G20_MUSB_REGS_H
#define MOTO_G20_MUSB_REGS_H
#define MU_FADDR       0x00
#define MU_POWER       0x01
#define MU_INTRTX      0x02
#define MU_INTRRX      0x04
#define MU_INTRTXE     0x06
#define MU_INTRRXE     0x08
#define MU_INTRUSB     0x0A
#define MU_INTRUSBE    0x0B
#define MU_INDEX       0x0E
#define MU_TXMAXP      0x10
#define MU_CSR0        0x12
#define MU_TXCSR       0x12
#define MU_RXMAXP      0x14
#define MU_RXCSR       0x16
#define MU_RXCOUNT     0x18
#define MU_TYPE0       0x1A
#define MU_NAKLIMIT0   0x1B
#define MU_RXTYPE      0x1C
#define MU_RXINTERVAL  0x1D
#define MU_CONFIGDATA  0x1F
#define MU_FIFO(Ep)    (0x20 + 4 * (Ep))
#define MU_DEVCTL      0x60
#define MU_TXFIFOSZ    0x62
#define MU_RXFIFOSZ    0x63
#define MU_TXFIFOADD   0x64
#define MU_RXFIFOADD   0x66
#define MU_HWVERS      0x6C
#define MU_EPINFO      0x78
#define MU_TXFUNC(Ep)  (0x80 + 8 * (Ep))
#define MU_TXHUB(Ep)   (0x82 + 8 * (Ep))
#define MU_TXPORT(Ep)  (0x83 + 8 * (Ep))
#define MU_RXFUNC(Ep)  (0x84 + 8 * (Ep))
#define MU_RXHUB(Ep)   (0x86 + 8 * (Ep))
#define MU_RXPORT(Ep)  (0x87 + 8 * (Ep))
#define MU_POWER_HSEN  0x20
#define MU_POWER_HSMODE 0x10
#define MU_POWER_RESET 0x08
#define MU_POWER_RESUME 0x04
#define MU_POWER_SUSPEND 0x02
#define MU_INTR_CONNECT 0x10
#define MU_INTR_DISCONNECT 0x20
#define MU_INTR_VBUSERR 0x80
#define MU_INTR_BABBLE 0x04
#define MU_DEV_FS      0x40
#define MU_DEV_LS      0x20
#define MU_DEV_VBUS    0x18
#define MU_DEV_HOST    0x04
#define MU_DEV_SESSION 0x01
#define MU_C0_RXREADY  0x0001
#define MU_C0_TXREADY  0x0002
#define MU_C0_STALL    0x0004
#define MU_C0_SETUP    0x0008
#define MU_C0_ERROR    0x0010
#define MU_C0_REQ      0x0020
#define MU_C0_STATUS   0x0040
#define MU_C0_NAK      0x0080
#define MU_C0_FLUSH    0x0100
#define MU_C0_NOPING   0x0800
#define MU_RX_READY   0x0001
#define MU_RX_ERROR   0x0004
#define MU_RX_DATAERR 0x0008
#define MU_RX_FLUSH   0x0010
#define MU_RX_REQ     0x0020
#define MU_RX_STALL   0x0040
#define MU_RX_CLRTOG  0x0080
#define MU_RX_TOGGLE  0x0200
#define MU_RX_WRTOG   0x0400
#define MU_RX_DISNYET 0x1000
#define MU_CFG_DYNFIFO 0x04
#define MU_CFG_BIGENDIAN 0x20
#define MU_OTG_EXT_CSR 0x34B
#define MU_HOST_FORCE 0x01
#define MU_DMA_EN_STATUS 0x1014
#define MU_DMA_CH_CFG(Ch) (0x1C04 + 0x20 * ((Ch) - 1))
#define MU_DMA_CH_INTR(Ch) (0x1C08 + 0x20 * ((Ch) - 1))
#define MU_TX_DMA_ENABLE 0x1000
#define MU_TX_DMA_MODE 0x0400
#define MU_RX_DMA_ENABLE 0x2000
#define MU_RX_DMA_MODE 0x0800
#endif
