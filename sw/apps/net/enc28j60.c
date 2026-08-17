/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * ENC28J60 driver. See enc28j60.h for the overall caveat -- this file
 * is the highest-confidence-but-still-unverified part of the
 * networking effort. Specific things worth checking first against the
 * datasheet if something doesn't work, roughly in likelihood order:
 *
 *   1. SPI timing/bit-bang itself (spi_xfer() below) -- if
 *      enc28j60_revision() doesn't come back as a small nonzero value
 *      (typically 1-6), start here, not deeper in the driver.
 *   2. The MAC address register byte order (MAADR1..MAADR6 below) --
 *      a well-known source of bugs in ENC28J60 drivers generally,
 *      since the register FILE address order does not match the MAC
 *      BYTE order.
 *   3. Bank/address encoding for individual registers (REG() macro
 *      and the constants below) -- transcribed from memory, not
 *      cross-checked against a datasheet PDF.
 *   4. The receive-buffer pointer errata workaround in
 *      enc28j60_recv() (ERXRDPT must always be odd) -- a real,
 *      documented ENC28J60 silicon errata, but the exact wording of
 *      the workaround is from memory.
 *
 * All SPI protocol timing happens here in software (see spibb_eth.v)
 * -- deliberately, so bugs found through hardware testing can be
 * fixed by editing this file, not by resynthesizing RTL.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "enc28j60.h"

// maskirq() is declared in ../../common/zeitlos.h (moved there once
// zgfx.c also needed it -- see that header's comment). Used below to
// protect SPI bit-bang transactions from interrupt preemption: a
// timer/UART IRQ firing mid-transaction (e.g. between raising SCK and
// reading MISO, or between bytes while CS is still held low)
// stretches that clock pulse by however long the interrupt takes to
// service -- a real SPI timing violation that can leave the chip in
// a bad state.

// -- SPI bit-bang primitives (mode 0,0: clock idle low, sample on
// rising edge, MSB first) -- same reg_eth bit layout as spibb_eth.v --

#define ETH_MISO()    (reg_eth & 0x01)
#define ETH_MOSI_H()  (reg_eth |= 0x02)
#define ETH_MOSI_L()  (reg_eth &= ~0x02)
#define ETH_SCK_H()   (reg_eth |= 0x04)
#define ETH_SCK_L()   (reg_eth &= ~0x04)
#define ETH_CS_H()    (reg_eth |= 0x08)   // deasserted -- chip select is active low
#define ETH_CS_L()    (reg_eth &= ~0x08)  // asserted

static void eth_delay_us(uint32_t n) {
	// approximate, not calibrated against real silicon timing --
	// tune if SPI communication is unreliable
	volatile uint32_t i;
	for (i = 0; i < n * 4; i++) ;
}

static void eth_delay_ms(uint32_t n) {
	while (n--) eth_delay_us(1000);
}

static uint8_t spi_xfer(uint8_t out) {

	uint8_t in = 0;

	for (int i = 7; i >= 0; i--) {
		if (out & (1 << i)) ETH_MOSI_H(); else ETH_MOSI_L();
		ETH_SCK_H();
		in <<= 1;
		if (ETH_MISO()) in |= 1;
		ETH_SCK_L();
	}

	return in;

}

// -- ENC28J60 SPI opcodes --

#define OP_RCR   0x00	// read control register
#define OP_RBM   0x3A	// read buffer memory (fixed "address" field)
#define OP_WCR   0x40	// write control register
#define OP_WBM   0x7A	// write buffer memory (fixed "address" field)
#define OP_BFS   0x80	// bit field set
#define OP_BFC   0xA0	// bit field clear
#define OP_SRC   0xFF	// system reset command

// -- register encoding: address(5) | bank(2) | needs-dummy-byte(1) --
//
// "needs dummy byte" applies to MAC and MII register groups: reading
// them via RCR returns one garbage byte before the real value (an
// ENC28J60 quirk, not an error). ETH-group registers don't have this.
// "common" registers (EIE/EIR/ESTAT/ECON2/ECON1, addr 0x1B-0x1F) are
// accessible from any bank and never need a bank switch.

#define ADDR_MASK  0x1F
#define BANK_MASK  0x60
#define SPRD_MASK  0x80

#define REG(addr, bank, sprd) ((addr) | ((bank) << 5) | ((sprd) ? SPRD_MASK : 0))

// bank 0
#define ERDPTL     REG(0x00, 0, 0)
#define ERDPTH     REG(0x01, 0, 0)
#define EWRPTL     REG(0x02, 0, 0)
#define EWRPTH     REG(0x03, 0, 0)
#define ETXSTL     REG(0x04, 0, 0)
#define ETXSTH     REG(0x05, 0, 0)
#define ETXNDL     REG(0x06, 0, 0)
#define ETXNDH     REG(0x07, 0, 0)
#define ERXSTL     REG(0x08, 0, 0)
#define ERXSTH     REG(0x09, 0, 0)
#define ERXNDL     REG(0x0A, 0, 0)
#define ERXNDH     REG(0x0B, 0, 0)
#define ERXRDPTL   REG(0x0C, 0, 0)
#define ERXRDPTH   REG(0x0D, 0, 0)

// bank 1
#define ERXFCON    REG(0x18, 1, 0)
#define EPKTCNT    REG(0x19, 1, 0)

// bank 2
#define MACON1     REG(0x00, 2, 1)
#define MACON3     REG(0x02, 2, 1)
#define MACON4     REG(0x03, 2, 1)
#define MABBIPG    REG(0x04, 2, 1)
#define MAIPGL     REG(0x06, 2, 1)
#define MAIPGH     REG(0x07, 2, 1)
#define MAMXFLL    REG(0x0A, 2, 1)
#define MAMXFLH    REG(0x0B, 2, 1)
#define MISTAT     REG(0x0A, 3, 1)	// note: bank 3, not 2 -- MII status
#define MICMD      REG(0x12, 2, 1)
#define MIREGADR   REG(0x14, 2, 1)
#define MIWRL      REG(0x16, 2, 1)
#define MIWRH      REG(0x17, 2, 1)
#define MIRDL      REG(0x18, 2, 1)
#define MIRDH      REG(0x19, 2, 1)

// bank 3 -- MAC address registers. NOTE the register FILE address
// order does not match the MAC BYTE order (a well-documented source
// of bugs): MAADR1 (register addr 0x04) holds MAC byte 0 (the first
// byte transmitted on the wire), ... MAADR6 (register addr 0x01)
// holds MAC byte 5.
#define MAADR5     REG(0x00, 3, 1)	// MAC byte 4
#define MAADR6     REG(0x01, 3, 1)	// MAC byte 5
#define MAADR3     REG(0x02, 3, 1)	// MAC byte 2
#define MAADR4     REG(0x03, 3, 1)	// MAC byte 3
#define MAADR1     REG(0x04, 3, 1)	// MAC byte 0
#define MAADR2     REG(0x05, 3, 1)	// MAC byte 1
#define EREVID     REG(0x12, 3, 0)

// common (any bank, no switch needed)
#define EIE        0x1B
#define EIR        0x1C
#define ESTAT      0x1D
#define ECON2      0x1E
#define ECON1      0x1F

// ECON1 bits
#define ECON1_BSEL_MASK  0x03
#define ECON1_RXEN       0x04
#define ECON1_TXRTS      0x08
#define ECON1_TXRST      0x80
// ECON2 bits
#define ECON2_PKTDEC     0x40
#define ECON2_AUTOINC    0x80
// EIR bits
#define EIR_RXERIF       0x01
#define EIR_TXERIF       0x02
#define EIR_TXIF         0x08

// PHY registers (accessed indirectly via MII, see eth_phy_write())
#define PHCON1     0x00
#define PHCON2     0x10

// -- buffer memory map --
// 8KB total (0x0000-0x1FFF), split into an RX ring and a TX area.
// RXSTOP_INIT is deliberately odd -- see enc28j60_init()'s comment on
// the ERXRDPT errata.
#define RXSTART_INIT   0x0000
#define RXSTOP_INIT    0x19FF
#define TXSTART_INIT   0x1A00
#define TXSTOP_INIT    0x1FFF

#define ETH_TX_TIMEOUT 200000	// loop iterations, not calibrated to real time

static uint8_t current_bank = 0xFF;	// force a bank select on first access
static uint16_t next_packet_ptr = RXSTART_INIT;
static uint8_t last_revision = 0;

// -- raw opcode primitives (no bank switching, no dummy-byte handling) --

static uint8_t eth_rcr_raw(uint8_t addr) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	uint8_t v;
	ETH_CS_L();
	spi_xfer(OP_RCR | (addr & ADDR_MASK));
	v = spi_xfer(0x00);
	ETH_CS_H();
	maskirq(old_mask);
	return v;
}

static void eth_wcr_raw(uint8_t addr, uint8_t data) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	ETH_CS_L();
	spi_xfer(OP_WCR | (addr & ADDR_MASK));
	spi_xfer(data);
	ETH_CS_H();
	maskirq(old_mask);
}

static void eth_bfs_raw(uint8_t addr, uint8_t mask) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	ETH_CS_L();
	spi_xfer(OP_BFS | (addr & ADDR_MASK));
	spi_xfer(mask);
	ETH_CS_H();
	maskirq(old_mask);
}

static void eth_bfc_raw(uint8_t addr, uint8_t mask) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	ETH_CS_L();
	spi_xfer(OP_BFC | (addr & ADDR_MASK));
	spi_xfer(mask);
	ETH_CS_H();
	maskirq(old_mask);
}

// -- bank switching (uses only the raw ECON1 primitives above, never
// the bank-aware helpers below, to avoid recursion) --

static void eth_set_bank(uint8_t reg) {

	uint8_t addr = reg & ADDR_MASK;
	if (addr >= 0x1B) return;	// common register, always accessible

	uint8_t bank = (reg >> 5) & 0x03;
	if (bank == current_bank) return;

	eth_bfc_raw(ECON1, ECON1_BSEL_MASK);
	if (bank) eth_bfs_raw(ECON1, bank);
	current_bank = bank;

}

// -- bank-aware, dummy-byte-aware register access --

static uint8_t eth_read_reg(uint8_t reg) {

	eth_set_bank(reg);

	uint8_t addr = reg & ADDR_MASK;

	if (reg & SPRD_MASK) {
		// MAC/MII register: the first byte returned is a dummy byte,
		// discard it
		uint32_t old_mask = maskirq(0xFFFFFFFF);
		uint8_t v;
		ETH_CS_L();
		spi_xfer(OP_RCR | addr);
		spi_xfer(0x00);	// dummy, discarded
		v = spi_xfer(0x00);
		ETH_CS_H();
		maskirq(old_mask);
		return v;
	}

	return eth_rcr_raw(addr);

}

static void eth_write_reg(uint8_t reg, uint8_t data) {
	eth_set_bank(reg);
	eth_wcr_raw(reg & ADDR_MASK, data);
}

static void eth_bit_field_set(uint8_t reg, uint8_t mask) {
	eth_set_bank(reg);
	eth_bfs_raw(reg & ADDR_MASK, mask);
}

static void eth_bit_field_clear(uint8_t reg, uint8_t mask) {
	eth_set_bank(reg);
	eth_bfc_raw(reg & ADDR_MASK, mask);
}

// L/H register pairs are always adjacent addresses within the same
// bank (e.g. ERDPTL=0x00, ERDPTH=0x01), so reg_l+1 correctly reaches
// the H register without needing a second named constant.

static void eth_write_reg16(uint8_t reg_l, uint16_t val) {
	eth_write_reg(reg_l, val & 0xFF);
	eth_write_reg(reg_l + 1, (val >> 8) & 0xFF);
}

static uint16_t eth_read_reg16(uint8_t reg_l) {
	uint16_t lo = eth_read_reg(reg_l);
	uint16_t hi = eth_read_reg(reg_l + 1);
	return lo | (hi << 8);
}

// -- buffer memory (RX/TX packet data) --
// relies on ECON2.AUTOINC (enabled in enc28j60_init()) so the chip's
// internal read/write pointers advance automatically, including
// wrapping at the RX buffer boundary.

static void eth_read_buffer(uint8_t *buf, uint16_t len) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	ETH_CS_L();
	spi_xfer(OP_RBM);
	for (uint16_t i = 0; i < len; i++)
		buf[i] = spi_xfer(0x00);
	ETH_CS_H();
	maskirq(old_mask);
}

static void eth_write_buffer(const uint8_t *buf, uint16_t len) {
	uint32_t old_mask = maskirq(0xFFFFFFFF);
	ETH_CS_L();
	spi_xfer(OP_WBM);
	for (uint16_t i = 0; i < len; i++)
		spi_xfer(buf[i]);
	ETH_CS_H();
	maskirq(old_mask);
}

// -- PHY (indirect access via MII registers) --

static void eth_phy_write(uint8_t phy_reg, uint16_t data) {
	eth_write_reg(MIREGADR, phy_reg);
	eth_write_reg16(MIWRL, data);
	// the write is triggered automatically once MIWRH is written;
	// wait for MISTAT.BUSY to clear before touching the MII interface
	// again
	while (eth_read_reg(MISTAT) & 0x01) ;
}

// -- public API --

bool enc28j60_init(const uint8_t mac[6]) {

	ETH_CS_H();
	ETH_SCK_L();

	// soft reset
	ETH_CS_L();
	spi_xfer(OP_SRC);
	ETH_CS_H();

	// Errata: wait for the oscillator to stabilize with a fixed delay
	// rather than trusting ESTAT.CLKRDY, which is documented as
	// unreliable on some silicon revisions.
	eth_delay_ms(2);

	current_bank = 0xFF;

	// -- receive buffer --
	eth_write_reg16(ERXSTL, RXSTART_INIT);
	eth_write_reg16(ERXNDL, RXSTOP_INIT);
	// Errata: ERXRDPT must always be programmed with an odd value.
	// RXSTOP_INIT is odd by construction; using it as the initial
	// read pointer satisfies this without special-casing.
	eth_write_reg16(ERXRDPTL, RXSTOP_INIT);
	next_packet_ptr = RXSTART_INIT;

	// -- transmit buffer -- (ETXND is set per-packet at send time)
	eth_write_reg16(ETXSTL, TXSTART_INIT);

	// auto-increment read/write pointers on buffer memory access,
	// including wrap-around at the RX buffer boundary
	eth_bit_field_set(ECON2, ECON2_AUTOINC);

	// receive filter: accept unicast (matching our MAC) + broadcast,
	// require a valid CRC
	eth_write_reg(ERXFCON, 0xA1);

	// -- MAC init -- full duplex.
	//
	// this was previously half duplex, chosen as what seemed like the
	// safer default (the ENC28J60's PHY doesn't autonegotiate duplex
	// with the link partner, and a full-duplex mismatch causes silent
	// packet loss). but half duplex on a modern switched network is
	// itself a mismatch in the other direction -- virtually all modern
	// switch ports run full duplex, and forcing half duplex against a
	// full-duplex link partner is a well-known source of excessive
	// collisions/retries. that's a real candidate for intermittent,
	// timing-dependent TX stalls: enc28j60_send()'s completion-polling
	// loop (below) would take much longer to resolve during a
	// collision/retry, and since each register read in that loop is
	// now wrapped in IRQ-masking SPI protection, a longer stall there
	// means a longer window with interrupts (including the timer IRQ
	// that drives scheduling) masked than originally assumed.
	eth_write_reg(MACON1, 0x01);		// MARXEN: enable frame reception
	eth_write_reg(MACON3, 0x33);		// pad<60B+auto CRC, frame length check, FULDPX=1
	eth_write_reg(MACON4, 0x00);		// DEFER not relevant in full duplex (no CSMA/CD deferral)
	eth_write_reg16(MAMXFLL, 1518);	// max frame length
	eth_write_reg(MABBIPG, 0x15);		// back-to-back inter-packet gap (full duplex value)
	eth_write_reg(MAIPGL, 0x12);		// non-back-to-back inter-packet gap

	eth_write_reg(MAADR1, mac[0]);
	eth_write_reg(MAADR2, mac[1]);
	eth_write_reg(MAADR3, mac[2]);
	eth_write_reg(MAADR4, mac[3]);
	eth_write_reg(MAADR5, mac[4]);
	eth_write_reg(MAADR6, mac[5]);

	// PHY: force full duplex (PHCON1.PDPXMD=1), matching MACON3.FULDPX=1 above.
	eth_phy_write(PHCON1, 0x0100);

	// PHY: disable half-duplex loopback (PHCON2.HDLDIS, bit 8). the
	// power-on default has this loopback ENABLED, meaning every frame
	// we transmit gets echoed straight back into our own RX path --
	// confirmed on real hardware (net.c's test broadcast frame showed
	// up as the very first "received" packet before this fix).
	// without this, there's no way to distinguish our own echoed
	// transmissions from real received traffic.
	eth_phy_write(PHCON2, 0x0100);

	last_revision = eth_read_reg(EREVID);

	eth_bit_field_set(ECON1, ECON1_RXEN);

	return (last_revision != 0x00 && last_revision != 0xFF);

}

uint8_t enc28j60_revision(void) {
	return last_revision;
}

void enc28j60_debug_dump(void) {
	uint8_t estat = eth_read_reg(ESTAT);
	uint8_t eir = eth_read_reg(EIR);
	uint8_t pktcnt = eth_read_reg(EPKTCNT);
	printf("net: [enc28j60] ESTAT=0x%02x EIR=0x%02x EPKTCNT=%d "
		"next_packet_ptr=0x%04x\n", estat, eir, pktcnt, next_packet_ptr);
}

uint16_t enc28j60_recv(uint8_t *buf, uint16_t maxlen) {

	if (eth_read_reg(EPKTCNT) == 0)
		return 0;

	eth_write_reg16(ERDPTL, next_packet_ptr);

	uint8_t hdr[6];
	eth_read_buffer(hdr, 6);

	uint16_t next_ptr = hdr[0] | ((uint16_t)hdr[1] << 8);
	uint16_t byte_count = hdr[2] | ((uint16_t)hdr[3] << 8);
	// hdr[4]/hdr[5] = receive status bits -- not checked in this
	// first pass; a truncated/errored frame is still handed to the
	// caller rather than silently dropped, since ip.c/etc will
	// naturally reject anything malformed anyway

	uint16_t to_read = (byte_count < maxlen) ? byte_count : maxlen;
	eth_read_buffer(buf, to_read);

	next_packet_ptr = next_ptr;

	// Errata: ERXRDPT must always be odd. documented workaround:
	// (next_ptr - 1), unless next_ptr == RXSTART_INIT, in which case
	// use RXSTOP_INIT instead (chosen odd -- see enc28j60_init()).
	uint16_t rdpt = (next_ptr == RXSTART_INIT) ? RXSTOP_INIT : next_ptr - 1;
	eth_write_reg16(ERXRDPTL, rdpt);

	eth_bit_field_set(ECON2, ECON2_PKTDEC);	// tell the chip we're done with this packet

	return to_read;

}

bool enc28j60_send(const uint8_t *buf, uint16_t len) {

	eth_write_reg16(EWRPTL, TXSTART_INIT);

	uint8_t ctrl = 0x00;	// per-packet control byte: use MAC defaults
	eth_write_buffer(&ctrl, 1);
	eth_write_buffer(buf, len);

	eth_write_reg16(ETXSTL, TXSTART_INIT);
	eth_write_reg16(ETXNDL, TXSTART_INIT + len);	// +1 (control byte) - 1 (inclusive end) = net +len

	eth_bit_field_clear(EIR, EIR_TXIF);	// clear any stale flag from a previous send

	eth_bit_field_set(ECON1, ECON1_TXRTS);	// start transmission

	uint32_t timeout;
	for (timeout = 0; timeout < ETH_TX_TIMEOUT; timeout++) {
		if (!(eth_read_reg(ECON1) & ECON1_TXRTS)) break;
	}

	if (eth_read_reg(ECON1) & ECON1_TXRTS) {
		// Errata: transmission can occasionally hang. documented
		// workaround: reset the transmit logic rather than waiting
		// forever.
		eth_bit_field_set(ECON1, ECON1_TXRST);
		eth_bit_field_clear(ECON1, ECON1_TXRST);
		eth_bit_field_clear(EIR, EIR_TXERIF);
		return false;
	}

	return !(eth_read_reg(EIR) & EIR_TXERIF);

}
