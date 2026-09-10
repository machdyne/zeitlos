/*------------------------------------------------------------------------/
/  Foolproof MMCv3/SDv1/SDv2 (in SPI mode) control module
/-------------------------------------------------------------------------/
/
/  Copyright (C) 2019, ChaN, all right reserved.
/
/ * This software is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/-------------------------------------------------------------------------/
  Features and Limitations:

  * Easy to Port Bit-banging SPI
    It uses only four GPIO pins. No complex peripheral needs to be used.

  * Platform Independent
    You need to modify only a few macros to control the GPIO port.

  * Low Speed
    The data transfer rate will be several times slower than hardware SPI.

  * No Media Change Detection
    Application program needs to perform a f_mount() after media change.

/-------------------------------------------------------------------------*/


#include "ff.h"		/* Obtains integer types for FatFs */
#include "diskio.h"	/* Common include file for FatFs and disk I/O layer */


/*-------------------------------------------------------------------------*/
/* Platform dependent macros and functions needed to be modified           */
/*-------------------------------------------------------------------------*/

#include "zeitlos.h"
#include "zsoc.h"			/* Z_SYSCLK_HZ, for dly_us() */		/* Include device specific declareation file here */
#include "../sdbench.h"		/* sd_stat_t, and the hooks sdbench uses */

/*--------------------------------------------------------------------------

   Hardware SPI back end (rtl/spisd.v)

   This driver used to toggle SCLK, MOSI and CS as GPIO, one wishbone
   cycle per edge. That made the SPI clock rate an emergent property of
   compiler codegen: it changed when the toolchain moved from GCC 8.2/
   rv32i to GCC 15.2/rv32im, it would change again with an instruction
   cache enabled, and it changed when a read-modify-write that looked
   redundant was removed -- that read was a full bus cycle, and
   deleting it both doubled SCLK and removed the card's data setup
   time.

   Now the shift register and clock divider live in gateware. Software
   writes a byte, polls BUSY, reads the byte that came back. Timing is
   identical on every board, at every optimisation level, under every
   compiler.

   Roughly 100 CPU cycles per BIT became roughly 48 per BYTE.

---------------------------------------------------------------------------*/

/* Exchange one byte, full duplex. SPI has no half-duplex mode: every
   byte sent produces a byte received, so a "send" discards the result
   and a "receive" sends 0xFF (which is what the card expects to see
   while it is the one talking). */
/* Set once at init: true when the gateware is "SPI1" (rtl/spim.v),
   which stalls a DATA access while busy and can transfer 32 bits at a
   time. Checked rather than assumed -- see Z_SPISD_MAGIC_V1 in
   zeitlos.h for what taking the fast path against an SPI0 bitstream
   would silently do. */
static int sd_spi_v1;

static BYTE spi_xchg (BYTE d)
{
	reg_spisd_data = d;

	/* On SPI1 the read below stalls until the transfer completes, so
	   the poll is not merely unnecessary, it is the cost being
	   removed: sdbench measured 190 CPU cycles per byte against 32
	   cycles of wire time, and this loop is most of the difference.
	   See docs/sdcard.md. */
	if (!sd_spi_v1)
		while (reg_spisd_status & Z_SPISD_BUSY) ;

	return (BYTE)(reg_spisd_data & 0xFF);
}

/* Chip select. Active low on the pin; the register takes 1 to ASSERT,
   so the polarity lives in the gateware rather than being open-coded
   at every call site the way the old GPIO version did it. */
#define CS_H()		do { reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div); } while(0)
#define CS_L()		do { reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) | Z_SPISD_CTRL_CS; } while(0)

/* Current divider, shadowed because CTRL holds CS and DIV in the same
   word and CS is changed far more often than the clock. */
static BYTE sd_div = Z_SPISD_DIV_INIT;

static void sd_set_speed (BYTE div)
{
	sd_div = div;
	reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) |
		((reg_spisd_ctrl & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);
}

/* -- hooks for sw/os/fs/sdbench.c --

   These exist so the benchmark can measure the layers separately. The
   whole point of `sdbench` is to answer "is the card slow, is the bus
   slow, or is the software slow", and that question cannot be answered
   from outside this file: spi_xchg() and the divider are both static,
   deliberately.

   sd_bench_xchg() clocks bytes with CS DEASSERTED, so nothing on the
   card sees them as a command -- it measures the wishbone round trip
   plus the gateware shift register and nothing else. That is the
   number to compare against the divider's theoretical rate: if they
   disagree, the bottleneck is the CPU's bus access, not SPI. */

void sd_bench_xchg (uint32_t n)
{
	while (n--) (void)spi_xchg(0xFF);
}

/* The same measurement through the WIDE path.
 *
 * Layer 0 exists to be the floor under layers 1-3, and it stopped
 * being one the moment rcvr_mmc() started using 32-bit transfers: a
 * benchmark reporting 285 KB/s for the raw exchange under a 848 KB/s
 * multi-block read is not measuring the same thing twice, it is
 * measuring two different paths and inviting the reader to compare
 * them. Both are reported now. */
void sd_bench_xchg32 (uint32_t n)
{
	uint32_t saved = reg_spisd_ctrl;

	reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) | Z_SPISD_CTRL_W32 |
		((saved & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);

	while (n >= 4) {
		reg_spisd_data = 0xFFFFFFFFu;
		(void)reg_spisd_data;
		n -= 4;
	}

	reg_spisd_ctrl = saved;

	while (n--) (void)spi_xchg(0xFF);
}

int sd_bench_is_v1 (void)
{
	return sd_spi_v1;
}

BYTE sd_bench_get_div (void)
{
	return sd_div;
}

void sd_bench_set_div (BYTE div)
{
	sd_set_speed(div);
}


/* -- Polling, and why there is no longer a dly_us() in the two wait
   loops below --

   Upstream's loops poll the card once and then sleep 100us. That number
   was chosen for a BIT-BANGED SPI port, where one poll byte cost around
   100 CPU cycles per BIT -- about 17us at 48MHz -- so a 100us sleep was
   the same order as the poll it was throttling.

   rtl/spim.v made that false. A poll byte is now a hardware transfer:
   eight SCLKs at DIV=1 is 0.67us, plus a handful of wishbone accesses
   either side. The sleep is over a hundred times the cost of the thing
   it throttles, and worse, it QUANTISES every wait to a 100us grid. A
   card that answers a data-token poll in 5us still costs 100us, and in
   a CMD18 multi-block read EVERY sector pays that, on top of the 384us
   the 512 bytes themselves take. See docs/sdcard.md.

   A tight poll is the right shape now: the SPI transfer is itself the
   throttle, and it polls exactly as fast as the card can answer.

   The timeouts still mean what they say only because they are measured
   with rdcycle rather than counted in iterations. An iteration count
   calibrated against a 100us sleep expires in a completely different
   wall-clock time without one, which is the trap this replaces rather
   than walks into -- 5000 iterations of a tight poll is about 3ms, not
   the 500ms the original comment claims.

   Same rdcycle caveat docs/filesystem.md already records applies: this
   measures WALL cycles, so it keeps meaning what it says only because
   the syscall dispatcher's preempt-deferral stops the holder being
   descheduled mid-operation. Nothing here changes that arrangement.

   SD_POLL_TIGHT=0 restores the original sleep, so the two can be
   measured against each other from one build flag rather than an edit:

       make -C sw/os clean && make -C sw/os EXTRA_CFLAGS=-DSD_POLL_TIGHT=0
*/
/* SD_POLL_TIGHT's default lives in ../sdbench.h, so that the benchmark
   reports the same value this file compiled against. */

/* Cheap counters, so `sdbench` (sw/os/fs/sdbench.c) can report WHERE
   the time went rather than only how long it took. Counted per poll
   and per sector, never per byte -- a per-byte counter would be a real
   cost inside the transfer loop it is trying to measure. */
sd_stat_t sd_stat;

static inline uint32_t sd_cycles (void)
{
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

/* 500ms is 24,000,000 cycles at 48MHz, which fits a uint32 with room
   to spare; anything longer would not, so do not raise these without
   checking that first. */
#define SD_US_TO_CYC(us)	((Z_SYSCLK_HZ / 1000000u) * (uint32_t)(us))

static
void dly_us (UINT n)	/* Delay n microseconds */
{
	/* Was three volatile reads per iteration, with a comment saying it
	   was calibrated for "avr-gcc -Os" -- a delay that depended on a
	   particular compiler, on a particular CPU, at a particular clock.
	   rdcycle is the real cycle counter, so this is n microseconds by
	   construction regardless of all three. */
	uint32_t start, now, target;

	__asm__ volatile ("rdcycle %0" : "=r"(start));
	target = (Z_SYSCLK_HZ / 1000000u) * (uint32_t)n;

	do {
		__asm__ volatile ("rdcycle %0" : "=r"(now));
	} while ((uint32_t)(now - start) < target);
}



/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

/* MMC/SD command (SPI mode) */
#define CMD0	(0)			/* GO_IDLE_STATE */
#define CMD1	(1)			/* SEND_OP_COND */
#define	ACMD41	(0x80+41)	/* SEND_OP_COND (SDC) */
#define CMD8	(8)			/* SEND_IF_COND */
#define CMD9	(9)			/* SEND_CSD */
#define CMD10	(10)		/* SEND_CID */
#define CMD12	(12)		/* STOP_TRANSMISSION */
#define CMD13	(13)		/* SEND_STATUS */
#define ACMD13	(0x80+13)	/* SD_STATUS (SDC) */
#define CMD16	(16)		/* SET_BLOCKLEN */
#define CMD17	(17)		/* READ_SINGLE_BLOCK */
#define CMD18	(18)		/* READ_MULTIPLE_BLOCK */
#define CMD23	(23)		/* SET_BLOCK_COUNT */
#define	ACMD23	(0x80+23)	/* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24	(24)		/* WRITE_BLOCK */
#define CMD25	(25)		/* WRITE_MULTIPLE_BLOCK */
#define CMD32	(32)		/* ERASE_ER_BLK_START */
#define CMD33	(33)		/* ERASE_ER_BLK_END */
#define CMD38	(38)		/* ERASE */
#define CMD55	(55)		/* APP_CMD */
#define CMD58	(58)		/* READ_OCR */


static
DSTATUS Stat = STA_NOINIT;	/* Disk status */

static
BYTE CardType;			/* b0:MMC, b1:SDv1, b2:SDv2, b3:Block addressing */



/*-----------------------------------------------------------------------*/
/* Transmit bytes to the card (hardware SPI master, see spi_xchg)        */
/*-----------------------------------------------------------------------*/

static
void xmit_mmc (
	const BYTE* buff,	/* Data to be sent */
	UINT bc				/* Number of bytes to send */
)
{
	/* Same wide path as rcvr_mmc(), same alignment caveat. */
	if (sd_spi_v1) {

		while (bc && ((uintptr_t)buff & 3)) {
			spi_xchg(*buff++);
			bc--;
		}

		if (bc >= 4) {
			reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) |
				Z_SPISD_CTRL_W32 |
				((reg_spisd_ctrl & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);

			while (bc >= 4) {
				reg_spisd_data = *(const uint32_t *)buff;
				(void)reg_spisd_data;   /* completes the transfer: the
										   read stalls until the shift
										   register is free */
				buff += 4;
				bc -= 4;
			}

			reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) |
				((reg_spisd_ctrl & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);
		}

		while (bc--) spi_xchg(*buff++);
		return;
	}

	do {
		spi_xchg(*buff++);	/* result discarded: the card is listening,
							   not talking, during a send */
	} while (--bc);
}




/*-----------------------------------------------------------------------*/
/* Receive bytes from the card (hardware SPI master, see spi_xchg)       */
/*-----------------------------------------------------------------------*/

static
void rcvr_mmc (
	BYTE *buff,	/* Pointer to read buffer */
	UINT bc		/* Number of bytes to receive */
)
{
	/* The wide path, when the gateware supports it and the buffer is
	   aligned: one bus access per four bytes instead of per byte.

	   Head and tail bytes go the narrow way. A 512-byte sector divides
	   by four, so in practice that is a few bytes at each end or none
	   at all -- but FatFs makes no alignment promise about the buffer
	   it hands us, and a misaligned 32-bit store on this core does not
	   fault, it silently writes the wrong bytes. */
	if (sd_spi_v1) {

		while (bc && ((uintptr_t)buff & 3)) {
			*buff++ = spi_xchg(0xFF);
			bc--;
		}

		if (bc >= 4) {
			reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) |
				Z_SPISD_CTRL_W32 |
				((reg_spisd_ctrl & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);

			while (bc >= 4) {
				reg_spisd_data = 0xFFFFFFFFu;
				*(uint32_t *)buff = reg_spisd_data;
				buff += 4;
				bc -= 4;
			}

			reg_spisd_ctrl = Z_SPISD_CTRL_DIV(sd_div) |
				((reg_spisd_ctrl & Z_SPISD_CTRL_CS) ? Z_SPISD_CTRL_CS : 0);
		}

		while (bc--) *buff++ = spi_xchg(0xFF);
		return;
	}

	do {
		*buff++ = spi_xchg(0xFF);	/* 0xFF holds MOSI high, which is
									   what the card expects while it
									   is the one driving MISO */
	} while (--bc);
}




/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

static
int wait_ready (void)	/* 1:OK, 0:Timeout */
{
	BYTE d;
	UINT tmr;


	uint32_t t0 = sd_cycles();
	uint32_t limit = SD_US_TO_CYC(500000u);		/* 500ms, as before */

	(void)tmr;

	do {
		rcvr_mmc(&d, 1);
		sd_stat.ready_polls++;
		if (d == 0xFF) return 1;
#if !SD_POLL_TIGHT
		dly_us(100);
#endif
	} while ((uint32_t)(sd_cycles() - t0) < limit);

	sd_stat.ready_timeouts++;

	return 0;
}



/*-----------------------------------------------------------------------*/
/* Deselect the card and release SPI bus                                 */
/*-----------------------------------------------------------------------*/

static
void deselect (void)
{
	BYTE d;

	CS_H();				/* Set CS# high */
	rcvr_mmc(&d, 1);	/* Dummy clock (force DO hi-z for multiple slave SPI) */
}



/*-----------------------------------------------------------------------*/
/* Select the card and wait for ready                                    */
/*-----------------------------------------------------------------------*/

static
int select (void)	/* 1:OK, 0:Timeout */
{
	BYTE d;

	CS_L();				/* Set CS# low */
	rcvr_mmc(&d, 1);	/* Dummy clock (force DO enabled) */
	if (wait_ready()) return 1;	/* Wait for card ready */

	deselect();
	return 0;			/* Failed */
}



/*-----------------------------------------------------------------------*/
/* Receive a data packet from the card                                   */
/*-----------------------------------------------------------------------*/

static
int rcvr_datablock (	/* 1:OK, 0:Failed */
	BYTE *buff,			/* Data buffer to store received data */
	UINT btr			/* Byte count */
)
{
	BYTE d[2];
	UINT tmr;


	uint32_t t0 = sd_cycles();
	uint32_t limit = SD_US_TO_CYC(100000u);		/* 100ms, as before */

	(void)tmr;

	/* This is the loop that mattered. In a CMD18 multi-block read it
	   runs once per SECTOR, and with the old 100us sleep it added that
	   sleep to every one of them -- roughly a quarter again on top of
	   the 384us the 512 bytes themselves cost at DIV=1, for a card
	   that had usually already answered. */
	d[0] = 0xFF;
	do {
		rcvr_mmc(d, 1);
		sd_stat.token_polls++;
		if (d[0] != 0xFF) break;
#if !SD_POLL_TIGHT
		dly_us(100);
#endif
	} while ((uint32_t)(sd_cycles() - t0) < limit);

	if (d[0] != 0xFE) {				/* If not valid data token, return with error */
		sd_stat.token_timeouts++;
		return 0;
	}

	rcvr_mmc(buff, btr);			/* Receive the data block into buffer */
	rcvr_mmc(d, 2);					/* Discard CRC */

	sd_stat.sectors_read++;

	return 1;						/* Return with success */
}



/*-----------------------------------------------------------------------*/
/* Send a data packet to the card                                        */
/*-----------------------------------------------------------------------*/

static
int xmit_datablock (	/* 1:OK, 0:Failed */
	const BYTE *buff,	/* 512 byte data block to be transmitted */
	BYTE token			/* Data/Stop token */
)
{
	BYTE d[2];


	if (!wait_ready()) return 0;

	d[0] = token;
	xmit_mmc(d, 1);				/* Xmit a token */
	if (token != 0xFD) {		/* Is it data token? */
		xmit_mmc(buff, 512);	/* Xmit the 512 byte data block to MMC */
		rcvr_mmc(d, 2);			/* Xmit dummy CRC (0xFF,0xFF) */
		rcvr_mmc(d, 1);			/* Receive data response */
		if ((d[0] & 0x1F) != 0x05)	/* If not accepted, return with error */
			return 0;
		sd_stat.sectors_written++;
	}

	return 1;
}



/*-----------------------------------------------------------------------*/
/* Send a command packet to the card                                     */
/*-----------------------------------------------------------------------*/

static
BYTE send_cmd (		/* Returns command response (bit7==1:Send failed)*/
	BYTE cmd,		/* Command byte */
	DWORD arg		/* Argument */
)
{
	BYTE n, d, buf[6];


	if (cmd & 0x80) {	/* ACMD<n> is the command sequense of CMD55-CMD<n> */
		cmd &= 0x7F;
		n = send_cmd(CMD55, 0);
		if (n > 1) return n;
	}

	/* Select the card and wait for ready except to stop multiple block read */
	if (cmd != CMD12) {
		deselect();
		if (!select()) return 0xFF;
	}

	sd_stat.commands++;

	/* Send a command packet */
	buf[0] = 0x40 | cmd;			/* Start + Command index */
	buf[1] = (BYTE)(arg >> 24);		/* Argument[31..24] */
	buf[2] = (BYTE)(arg >> 16);		/* Argument[23..16] */
	buf[3] = (BYTE)(arg >> 8);		/* Argument[15..8] */
	buf[4] = (BYTE)arg;				/* Argument[7..0] */
	n = 0x01;						/* Dummy CRC + Stop */
	if (cmd == CMD0) n = 0x95;		/* (valid CRC for CMD0(0)) */
	if (cmd == CMD8) n = 0x87;		/* (valid CRC for CMD8(0x1AA)) */
	buf[5] = n;
	xmit_mmc(buf, 6);

	/* Receive command response */
	if (cmd == CMD12) rcvr_mmc(&d, 1);	/* Skip a stuff byte when stop reading */
	n = 10;								/* Wait for a valid response in timeout of 10 attempts */
	do
		rcvr_mmc(&d, 1);
	while ((d & 0x80) && --n);

	return d;			/* Return with the response value */
}



/*--------------------------------------------------------------------------

   Public Functions

---------------------------------------------------------------------------*/


/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS sd_disk_status (
	BYTE drv			/* Drive number (always 0) */
)
{
	if (drv) return STA_NOINIT;

	return Stat;
}



/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */
/*-----------------------------------------------------------------------*/

DSTATUS sd_disk_initialize (
	BYTE drv		/* Physical drive nmuber (0) */
)
{
	BYTE n, ty, cmd, buf[4];
	UINT tmr;
	DSTATUS s;


	if (drv) return RES_NOTRDY;

	dly_us(10000);			/* 10ms */
	/* No pin setup: rtl/spisd.v owns the pins and comes out of reset
	   with CS deasserted and SCLK idle low. Cards must be clocked at
	   400kHz or below until they leave idle state, hence the slow
	   divider here; it is raised once initialisation succeeds. */
	/* Which gateware is underneath us. Checked once, here, before any
	   transfer: everything below assumes one answer or the other and
	   getting it wrong is silent corruption rather than a failure.
	   See Z_SPISD_MAGIC_V1 in zeitlos.h. */
	sd_spi_v1 = (reg_spisd_magic == Z_SPISD_MAGIC_V1);

	sd_set_speed(Z_SPISD_DIV_INIT);
	CS_H();

	for (n = 10; n; n--) rcvr_mmc(buf, 1);	/* Apply 80 dummy clocks and the card gets ready to receive command */

	ty = 0;
	if (send_cmd(CMD0, 0) == 1) {			/* Enter Idle state */
		if (send_cmd(CMD8, 0x1AA) == 1) {	/* SDv2? */
			rcvr_mmc(buf, 4);							/* Get trailing return value of R7 resp */
			if (buf[2] == 0x01 && buf[3] == 0xAA) {		/* The card can work at vdd range of 2.7-3.6V */
				for (tmr = 1000; tmr; tmr--) {			/* Wait for leaving idle state (ACMD41 with HCS bit) */
					if (send_cmd(ACMD41, 1UL << 30) == 0) break;
					dly_us(1000);
				}
				if (tmr && send_cmd(CMD58, 0) == 0) {	/* Check CCS bit in the OCR */
					rcvr_mmc(buf, 4);
					ty = (buf[0] & 0x40) ? CT_SDC2 | CT_BLOCK : CT_SDC2;	/* SDv2+ */
				}
			}
		} else {							/* SDv1 or MMCv3 */
			if (send_cmd(ACMD41, 0) <= 1) 	{
				ty = CT_SDC2; cmd = ACMD41;	/* SDv1 */
			} else {
				ty = CT_MMC3; cmd = CMD1;	/* MMCv3 */
			}
			for (tmr = 1000; tmr; tmr--) {			/* Wait for leaving idle state */
				if (send_cmd(cmd, 0) == 0) break;
				dly_us(1000);
			}
			if (!tmr || send_cmd(CMD16, 512) != 0)	/* Set R/W block length to 512 */
				ty = 0;
		}
	}
	CardType = ty;

	/* Initialisation is done, so leave the mandatory 400kHz behind.
	   Everything above this point had to be slow because a card in idle
	   state is only specified to 400kHz; from here it will take a real
	   clock, and this is where the speed of the whole filesystem comes
	   from. Lower Z_SPISD_DIV_FAST (zeitlos.h) for more, but verify
	   against the slowest card you care about -- 24MHz is the top of SPI
	   mode and not every card reaches it. */
	if (ty) sd_set_speed(Z_SPISD_DIV_FAST);

	s = ty ? 0 : STA_NOINIT;

	Stat = s;

	deselect();

	return s;
}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT sd_disk_read (
	BYTE drv,			/* Physical drive nmuber (0) */
	BYTE *buff,			/* Pointer to the data buffer to store read data */
	LBA_t sector,		/* Start sector number (LBA) */
	UINT count			/* Sector count (1..128) */
)
{
	BYTE cmd;
	DWORD sect = (DWORD)sector;


	if (sd_disk_status (drv) & STA_NOINIT) return RES_NOTRDY;
	if (!(CardType & CT_BLOCK)) sect *= 512;	/* Convert LBA to byte address if needed */

	cmd = count > 1 ? CMD18 : CMD17;			/*  READ_MULTIPLE_BLOCK : READ_SINGLE_BLOCK */
	if (send_cmd(cmd, sect) == 0) {
		do {
			if (!rcvr_datablock(buff, 512)) break;
			buff += 512;
		} while (--count);
		if (cmd == CMD18) send_cmd(CMD12, 0);	/* STOP_TRANSMISSION */
	}
	deselect();

	return count ? RES_ERROR : RES_OK;
}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT sd_disk_write (
	BYTE drv,			/* Physical drive nmuber (0) */
	const BYTE *buff,	/* Pointer to the data to be written */
	LBA_t sector,		/* Start sector number (LBA) */
	UINT count			/* Sector count (1..128) */
)
{
	DWORD sect = (DWORD)sector;


	if (sd_disk_status (drv) & STA_NOINIT) return RES_NOTRDY;
	if (!(CardType & CT_BLOCK)) sect *= 512;	/* Convert LBA to byte address if needed */

	if (count == 1) {	/* Single block write */
		if ((send_cmd(CMD24, sect) == 0)	/* WRITE_BLOCK */
			&& xmit_datablock(buff, 0xFE))
			count = 0;
	}
	else {				/* Multiple block write */
		if (CardType & CT_SDC) send_cmd(ACMD23, count);
		if (send_cmd(CMD25, sect) == 0) {	/* WRITE_MULTIPLE_BLOCK */
			do {
				if (!xmit_datablock(buff, 0xFC)) break;
				buff += 512;
			} while (--count);
			if (!xmit_datablock(0, 0xFD))	/* STOP_TRAN token */
				count = 1;
		}
	}
	deselect();

	return count ? RES_ERROR : RES_OK;
}


/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT sd_disk_ioctl (
	BYTE drv,		/* Physical drive nmuber (0) */
	BYTE ctrl,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	DRESULT res;
	BYTE n, csd[16];
	DWORD cs;


	if (sd_disk_status (drv) & STA_NOINIT) return RES_NOTRDY;	/* Check if card is in the socket */

	res = RES_ERROR;
	switch (ctrl) {
		case CTRL_SYNC :		/* Make sure that no pending write process */
			if (select()) res = RES_OK;
			break;

		case GET_SECTOR_COUNT :	/* Get number of sectors on the disk (DWORD) */
			if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
				if ((csd[0] >> 6) == 1) {	/* SDC ver 2.00 */
					cs = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
					*(LBA_t*)buff = cs << 10;
				} else {					/* SDC ver 1.XX or MMC */
					n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
					cs = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
					*(LBA_t*)buff = cs << (n - 9);
				}
				res = RES_OK;
			}
			break;

		case GET_BLOCK_SIZE :	/* Get erase block size in unit of sector (DWORD) */
			*(DWORD*)buff = 128;
			res = RES_OK;
			break;

		default:
			res = RES_PARERR;
	}

	deselect();

	return res;
}


