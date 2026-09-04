/*
 * Host test for sw/common/zi2c.c and sw/common/zspi.c.
 *
 *   cc -std=gnu99 -I sw/common -o /tmp/t \
 *      sw/common/tests/test_bitbang.c \
 *      sw/common/zi2c.c sw/common/zspi.c sw/common/zgpio.c && /tmp/t
 *
 * Unlike test_gfx_region.c next door, this does NOT lift the code
 * under test into the test file -- it compiles the real .c files
 * unmodified. That is worth the machinery below, because an I2C
 * master is a state machine: a lifted copy would drift, and a test
 * passing against a stale copy of a state machine is worse than no
 * test at all.
 *
 * -- How the hardware is faked --
 *
 * Two things have to work for the real sources to run here.
 *
 * The CYCLE COUNTER: sw/common/zcycles.h has a non-RISC-V branch that
 * returns an advancing counter, so the delay loops terminate and every
 * timeout path stays reachable. Delays are meaningless off-target,
 * which is fine -- there is nothing here to be slow for.
 *
 * The GPIO BLOCK: mapped with mmap() at the address sw/common/zgpio.h
 * expects, then made read-only, so every STORE faults. The SIGSEGV
 * handler unprotects, sets the x86 trap flag and returns; the store
 * completes; the SIGTRAP handler folds the write into the model and
 * re-protects. Reads never fault and cost nothing.
 *
 * That is what makes this faithful rather than approximate. OUTSET,
 * OUTCLR, DIRSET and DIRCLR are write-1-to-modify aliases in
 * rtl/gpio.v, and plain memory cannot emulate them -- the master's
 * stores would pile up in the alias words and never reach DIR or OUT.
 * Trapping each store applies it exactly when the hardware would, in
 * order, one at a time. It also means the simulated slave below
 * observes every edge in the right sequence without the master having
 * to cooperate in any way.
 *
 * Linux and x86-64 only; prints a skip and exits 77 elsewhere.
 *
 * -- What this proves, and what it does not --
 *
 * The bus model is FUNCTIONAL, NOT ELECTRICAL. It has pull-ups and
 * open-drain contention, so the protocol logic is exercised honestly.
 * It has no rise time, no capacitance and no clock skew, so it cannot
 * catch a timing bug -- that needs a scope and a real PMOD.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>

#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
#define CAN_RUN 1
#else
#define CAN_RUN 0
#endif

#include "zgpio.h"
#include "zi2c.h"
#include "zspi.h"

#if !CAN_RUN

int main(void) {
	printf("test_bitbang: skipped (needs Linux/x86-64 for store trapping)\n");
	return 77;
}

#else

static int fails;

#define CK(what, got, want) do { \
	long _g = (long)(got), _w = (long)(want); \
	if (_g != _w) { \
		printf("FAIL %-34s got %ld want %ld\n", what, _g, _w); fails++; \
	} else printf("ok   %-34s %ld\n", what, _g); \
} while (0)

#define MAP_BASE ((void *)0xe0000000UL)
#define MAP_LEN  0x2000u			// board regs, and the ports at +0x1000

#define NPORTS 2

#define PORT_I2C 0
#define PORT_SPI 1

#define I2C_SCL 0
#define I2C_SDA 1

#define SLAVE_ADDR 0x3c

// Bits a simulated device is actively pulling down on PORT_I2C.
static uint8_t dev_pulldown;

// Bits held down by a FAULT rather than by the slave -- a short, a
// missing pull-up, a wedged part. Kept separate from dev_pulldown
// because the slave clears its own pull-down on every START and STOP,
// and a fault that a START clears is not a fault.
static uint8_t stuck_mask;

// MISO tied to MOSI on PORT_SPI, so a transfer reads back what it
// sent. A loopback with no wire; see test_spi().
static bool spi_loopback;
#define SPI_MOSI_PIN 1
#define SPI_MISO_PIN 2

static void slave_step(void);

// ---------------------------------------------------------------
// the wire
// ---------------------------------------------------------------

// Fold the write-1-to-modify aliases into DIR and OUT, then recompute
// IN -- which is the whole of rtl/gpio.v's behaviour software can see.
//
// A line reads LOW if this chip is driving it low or a device is
// pulling it down, and HIGH otherwise. That last clause is the
// pull-up, and it is why a floating pin reads 1 here exactly as it
// does on a board (release/hw/pmods/gpio.spec sets PULLMODE=UP). A
// model that returned 0 for a floating pin would let an I2C master
// pass here and read nothing but zeros from real hardware.
static void gpio_apply(void) {

	int p;

	for (p = 0; p < NPORTS; p++) {

		volatile uint32_t *r = z_gpio_reg((uint32_t)p, 0);

		uint32_t dir = r[Z_GPIO_REG_DIR / 4];
		uint32_t out = r[Z_GPIO_REG_OUT / 4];
		uint32_t level;

		out |= r[Z_GPIO_REG_OUTSET / 4];
		out &= ~r[Z_GPIO_REG_OUTCLR / 4];
		dir |= r[Z_GPIO_REG_DIRSET / 4];
		dir &= ~r[Z_GPIO_REG_DIRCLR / 4];

		dir &= 0xffu;
		out &= 0xffu;

		r[Z_GPIO_REG_DIR / 4] = dir;
		r[Z_GPIO_REG_OUT / 4] = out;

		// The aliases are momentary in hardware -- there is no such
		// register for them to accumulate in -- so clear once applied.
		r[Z_GPIO_REG_OUTSET / 4] = 0;
		r[Z_GPIO_REG_OUTCLR / 4] = 0;
		r[Z_GPIO_REG_DIRSET / 4] = 0;
		r[Z_GPIO_REG_DIRCLR / 4] = 0;

		level = ~((~out & dir)
			| (p == PORT_I2C ? (uint32_t)(dev_pulldown | stuck_mask) : 0u));

		level &= 0xffu;

		if (p == PORT_SPI && spi_loopback) {
			// MISO reads whatever MOSI is driving.
			uint32_t mosi = (out >> SPI_MOSI_PIN) & (dir >> SPI_MOSI_PIN) & 1u;
			level &= ~(1u << SPI_MISO_PIN);
			level |= mosi << SPI_MISO_PIN;
		}

		r[Z_GPIO_REG_IN / 4] = level;

	}

}

// ---------------------------------------------------------------
// store trapping
// ---------------------------------------------------------------

static void on_segv(int sig, siginfo_t *si, void *uc) {
	ucontext_t *c = uc;
	(void)sig; (void)si;
	// Let the faulting store through, then single-step so control
	// comes back immediately after it.
	mprotect(MAP_BASE, MAP_LEN, PROT_READ | PROT_WRITE);
	c->uc_mcontext.gregs[REG_EFL] |= 0x100;			// TF
}

static void on_trap(int sig, siginfo_t *si, void *uc) {
	ucontext_t *c = uc;
	(void)sig; (void)si;
	c->uc_mcontext.gregs[REG_EFL] &= ~0x100;
	// The store has landed. Apply it and let whatever is on the other
	// end react, BEFORE re-protecting -- both write to this page.
	gpio_apply();
	slave_step();
	gpio_apply();
	mprotect(MAP_BASE, MAP_LEN, PROT_READ);
}

// Changes made from test code are ordinary writes to ordinary
// variables and trap nothing, so they need pushing into the model by
// hand.
static void settle(void) {
	mprotect(MAP_BASE, MAP_LEN, PROT_READ | PROT_WRITE);
	gpio_apply();
	mprotect(MAP_BASE, MAP_LEN, PROT_READ);
}

// ---------------------------------------------------------------
// a simulated I2C slave
// ---------------------------------------------------------------
//
// Bit-level and edge-driven, so it exercises the master the way a real
// part would: it decides for itself when to acknowledge, drives data
// on falling edges, and releases the line when it is not driving.
//
// Four registers behind a register pointer, which is the shape of
// almost every real I2C device and is what makes the write-then-
// repeated-START-read path worth testing.

static struct {
	int     state;		// 0 idle, 1 address, 2 write data, 3 read data
	int     bit;
	uint8_t shift;
	bool    reading;
	bool    selected;
	uint8_t regs[4];
	int     regptr;
	int     wrcount;
	int     stops;
	int     starts;
} sl;

static bool prev_scl = true, prev_sda = true;

static void sda_pull(bool low) {
	if (low) dev_pulldown |= (uint8_t)(1u << I2C_SDA);
	else dev_pulldown &= (uint8_t)~(1u << I2C_SDA);
}

static void slave_step(void) {

	volatile uint32_t *r = z_gpio_reg(PORT_I2C, 0);
	uint32_t in = r[Z_GPIO_REG_IN / 4];

	bool scl = ((in >> I2C_SCL) & 1) != 0;
	bool sda = ((in >> I2C_SDA) & 1) != 0;

	if (scl && prev_scl && prev_sda && !sda) {			// START
		sl.state = 1; sl.bit = 0; sl.shift = 0;
		sl.selected = false; sl.reading = false;
		sl.starts++;
		sda_pull(false);
		goto done;
	}

	if (scl && prev_scl && !prev_sda && sda) {			// STOP
		sl.state = 0; sl.selected = false;
		sl.stops++;
		sda_pull(false);
		goto done;
	}

	if (!prev_scl && scl) {								// rising: sample

		// Only the eight data bits. Shifting on the ninth would fold
		// the acknowledgement into the byte, which is exactly the bug
		// this guard exists for.
		if ((sl.state == 1 || sl.state == 2) && sl.bit < 8) {
			sl.shift = (uint8_t)((sl.shift << 1) | (sda ? 1 : 0));
			sl.bit++;
		}
		else if (sl.state == 3 && sl.bit == 9 && sda) {
			// The master NACKed the byte we just sent, which means
			// "that was the last one". A real device stops driving
			// here; one that carried on would hold SDA down through
			// the STOP and wedge the bus. Modelling that faithfully
			// is what makes the final-NACK behaviour of the master
			// worth testing at all.
			sl.state = 0;
			sda_pull(false);
		}

	} else if (prev_scl && !scl) {						// falling: drive

		if (sl.state == 1) {

			if (sl.bit == 8) {
				sl.selected = ((sl.shift >> 1) == SLAVE_ADDR);
				sl.reading = (sl.shift & 1) != 0;
				sda_pull(sl.selected);			// ack by pulling low
				sl.bit = 9;
			}
			else if (sl.bit == 9) {
				sda_pull(false);				// release the ack
				sl.bit = 0; sl.shift = 0;
				if (!sl.selected) {
					sl.state = 0;
				} else if (sl.reading) {
					sl.state = 3;
					sl.shift = sl.regs[sl.regptr & 3];
					// present the first data bit on this same edge
					sda_pull((sl.shift & 0x80) == 0);
					sl.shift = (uint8_t)(sl.shift << 1);
					sl.bit = 1;
				} else {
					sl.state = 2;
				}
			}

		}
		else if (sl.state == 2) {

			if (sl.bit == 8) {
				// The first byte after the address is the register
				// pointer; anything after it is data.
				if (sl.wrcount == 0) sl.regptr = sl.shift & 3;
				else sl.regs[sl.regptr & 3] = sl.shift;
				sl.wrcount++;
				sda_pull(true);
				sl.bit = 9;
			}
			else if (sl.bit == 9) {
				sda_pull(false);
				sl.bit = 0; sl.shift = 0;
			}

		}
		else if (sl.state == 3) {

			if (sl.bit == 8) {
				sda_pull(false);		// release for the master's ack
				sl.bit = 9;
			} else {
				if (sl.bit == 9) {
					sl.regptr++;
					sl.shift = sl.regs[sl.regptr & 3];
					sl.bit = 0;
				}
				sda_pull((sl.shift & 0x80) == 0);
				sl.shift = (uint8_t)(sl.shift << 1);
				sl.bit++;
			}

		}

	}

done:
	prev_scl = scl;
	prev_sda = sda;

}

static void reset_slave(void) {
	memset(&sl, 0, sizeof(sl));
	dev_pulldown = 0;
	stuck_mask = 0;
	prev_scl = true;
	prev_sda = true;
	sl.regs[0] = 0xa5;
	sl.regs[1] = 0x3c;
	sl.regs[2] = 0x5a;
	sl.regs[3] = 0xff;
	settle();
}

// ---------------------------------------------------------------

static void test_spi(void) {

	z_spi_t s;
	uint8_t rx[2];
	int mode;

	memset(&s, 0, sizeof(s));
	s.sck_port = PORT_SPI;  s.sck_pin = 0;
	s.mosi_port = PORT_SPI; s.mosi_pin = 1;
	s.miso_port = Z_SPI_NO_PIN;
	s.cs_port = PORT_SPI;   s.cs_pin = 3;
	s.mode = 0;
	s.khz = 1000;

	CK("spi init mode 0", z_spi_init(&s), 1);
	CK("spi mode0 sclk idles low", z_gpio_out_get(PORT_SPI) & 1, 0);
	CK("spi cs idles deasserted", (z_gpio_out_get(PORT_SPI) >> 3) & 1, 1);
	CK("spi drives sck/mosi/cs", z_gpio_dir_get(PORT_SPI) & 0x0b, 0x0b);

	s.mode = 2;
	CK("spi init mode 2", z_spi_init(&s), 1);
	CK("spi mode2 sclk idles high", z_gpio_out_get(PORT_SPI) & 1, 1);

	s.mode = 4;
	CK("spi rejects mode 4", z_spi_init(&s), 0);

	s.mode = 0;
	z_spi_init(&s);
	z_spi_select(&s, true);
	CK("spi cs asserted low", (z_gpio_out_get(PORT_SPI) >> 3) & 1, 0);
	z_spi_select(&s, false);
	CK("spi cs released high", (z_gpio_out_get(PORT_SPI) >> 3) & 1, 1);

	s.cs_active_high = true;
	z_spi_init(&s);
	CK("spi active-high cs idles low", (z_gpio_out_get(PORT_SPI) >> 3) & 1, 0);
	z_spi_select(&s, true);
	CK("spi active-high cs asserted", (z_gpio_out_get(PORT_SPI) >> 3) & 1, 1);
	z_spi_select(&s, false);

	// No MISO pin: reads come back 0xff, matching an unconnected
	// pull-up input. A device that should have answered and did not
	// therefore looks like "nothing there" rather than like data.
	s.cs_active_high = false;
	z_spi_init(&s);
	z_spi_xfer(&s, NULL, rx, 2);
	CK("spi no-miso reads ff", rx[0], 0xff);
	CK("spi no-miso reads ff (2)", rx[1], 0xff);

	// SCLK must return to its idle level, or the next transfer opens
	// with a spurious edge and every byte is off by a bit.
	CK("spi sclk back to idle", z_gpio_out_get(PORT_SPI) & 1, 0);

	// MISO on its own pin, tied to MOSI by the wire model above. This
	// is the end-to-end check: bit order and both CPHA paths, with
	// the sampled value coming back through a real read.
	//
	// (Pointing MISO at the MOSI pin instead does NOT work, and the
	// reason is worth knowing: z_spi_init() configures MISO as an
	// input last, so it would turn MOSI back into an input and every
	// byte would read as the pull-up. A degenerate configuration, but
	// a silent one.)
	spi_loopback = true;
	s.miso_port = PORT_SPI; s.miso_pin = SPI_MISO_PIN;
	for (mode = 0; mode <= 3; mode++) {
		char name[40];
		s.mode = (uint8_t)mode;
		z_spi_init(&s);
		snprintf(name, sizeof(name), "spi loopback mode %d", mode);
		CK(name, z_spi_xfer8(&s, 0xb5), 0xb5);
	}

	s.lsb_first = true;
	s.mode = 0;
	z_spi_init(&s);
	CK("spi loopback lsb first", z_spi_xfer8(&s, 0x1c), 0x1c);

	spi_loopback = false;
	settle();

}

static void test_i2c(void) {

	z_i2c_t b;
	uint8_t v = 0, buf[2], found[8];
	int n;
	z_i2c_rv rv;

	memset(&b, 0, sizeof(b));
	b.scl_port = PORT_I2C; b.scl_pin = I2C_SCL;
	b.sda_port = PORT_I2C; b.sda_pin = I2C_SDA;
	b.khz = 100;
	b.timeout_us = 1000;

	reset_slave();
	CK("i2c init on idle bus", z_i2c_init(&b), Z_I2C_OK);
	CK("i2c leaves both lines released", z_gpio_dir_get(PORT_I2C) & 3, 0);

	// A line still low with nothing driving it is a real answer, not
	// a failure to initialise -- the caller can recover and retry.
	stuck_mask = (uint8_t)(1u << I2C_SDA);
	settle();
	CK("i2c init reports stuck sda", z_i2c_init(&b), Z_I2C_BUSY);
	stuck_mask = 0;
	settle();

	reset_slave();
	z_i2c_init(&b);
	rv = z_i2c_write(&b, SLAVE_ADDR, NULL, 0, true);
	CK("probe present device", rv, Z_I2C_OK);
	CK("  exactly one start", sl.starts, 1);
	CK("  exactly one stop", sl.stops, 1);

	reset_slave();
	z_i2c_init(&b);
	rv = z_i2c_write(&b, 0x11, NULL, 0, true);
	CK("probe absent device nacks", rv, Z_I2C_NACK);
	// A STOP even on NACK: leaving the bus held would make the next
	// caller fail too, turning one missing device into a dead bus.
	CK("  nack still sent a stop", sl.stops, 1);

	reset_slave();
	z_i2c_init(&b);
	buf[0] = 1; buf[1] = 0x5e;
	CK("write register", z_i2c_write(&b, SLAVE_ADDR, buf, 2, true), Z_I2C_OK);
	CK("  slave stored the value", sl.regs[1], 0x5e);

	reset_slave();
	z_i2c_init(&b);
	CK("reg read 1", z_i2c_reg_read8(&b, SLAVE_ADDR, 1, &v), Z_I2C_OK);
	CK("  value", v, 0x3c);

	reset_slave();
	z_i2c_init(&b);
	CK("reg read 0", z_i2c_reg_read8(&b, SLAVE_ADDR, 0, &v), Z_I2C_OK);
	CK("  value", v, 0xa5);

	// The repeated START is the point of this one: a START for the
	// write, another for the read, and exactly one STOP at the end.
	reset_slave();
	z_i2c_init(&b);
	buf[0] = 0;
	CK("write_read two bytes",
		z_i2c_write_read(&b, SLAVE_ADDR, buf, 1, buf, 2), Z_I2C_OK);
	CK("  first byte", buf[0], 0xa5);
	CK("  second byte", buf[1], 0x3c);
	CK("  two starts", sl.starts, 2);
	CK("  one stop, at the end", sl.stops, 1);

	reset_slave();
	z_i2c_init(&b);
	n = z_i2c_scan(&b, found, 8);
	CK("scan finds one device", n, 1);
	CK("  at the right address", found[0], SLAVE_ADDR);

	CK("measured khz is reported", z_i2c_measured_khz(&b) > 0, 1);

	// A line that never comes back up is a TIMEOUT, not a NACK. That
	// distinction is why this library returns an enum: a NACK means
	// the bus works and nobody answered, which is what a scan is made
	// of, and a timeout means no pull-up or a short.
	reset_slave();
	z_i2c_init(&b);
	stuck_mask = (uint8_t)(1u << I2C_SCL);
	settle();
	rv = z_i2c_write(&b, SLAVE_ADDR, NULL, 0, true);
	CK("stuck scl is timeout not nack", rv, Z_I2C_TIMEOUT);
	stuck_mask = 0;
	settle();

	// The unwedge, and the one failure a master can actually fix.
	reset_slave();
	z_i2c_init(&b);
	stuck_mask = (uint8_t)(1u << I2C_SDA);
	settle();
	CK("recover fails while sda held", z_i2c_recover(&b), Z_I2C_TIMEOUT);
	stuck_mask = 0;
	settle();
	CK("recover succeeds once released", z_i2c_recover(&b), Z_I2C_OK);

	CK("strerror nack", strcmp(z_i2c_strerror(Z_I2C_NACK), "nack"), 0);
	CK("strerror timeout", strcmp(z_i2c_strerror(Z_I2C_TIMEOUT), "timeout"), 0);
	CK("strerror busy", strcmp(z_i2c_strerror(Z_I2C_BUSY), "busy"), 0);

}

int main(void) {

	struct sigaction a;
	void *m;

	m = mmap(MAP_BASE, MAP_LEN, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (m == MAP_FAILED) {
		printf("test_bitbang: skipped (cannot map 0xe0000000)\n");
		return 77;
	}

	memset(m, 0, MAP_LEN);

	// Enough of the block's identity for z_gpio_port_count() to
	// answer -- zgpio.c checks the magic and the signature before it
	// does anything at all.
	*(volatile uint32_t *)0xe0000008 = Z_GPIO_MAGIC;
	*(volatile uint32_t *)0xe000000c =
		((uint32_t)Z_GPIO_CONFIG_SIG << 16) | NPORTS;

	memset(&a, 0, sizeof(a));
	a.sa_flags = SA_SIGINFO;
	a.sa_sigaction = on_segv;
	sigaction(SIGSEGV, &a, NULL);
	a.sa_sigaction = on_trap;
	sigaction(SIGTRAP, &a, NULL);

	gpio_apply();
	mprotect(MAP_BASE, MAP_LEN, PROT_READ);

	test_spi();
	printf("\n");
	test_i2c();

	printf(fails ? "\nFAIL (%d)\n" : "\nPASS\n", fails);

	return fails != 0;

}

#endif
