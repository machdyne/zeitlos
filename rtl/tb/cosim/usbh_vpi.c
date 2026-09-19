/*
 * Zeitlos -- simulation only.
 *
 * Co-simulation bridge: runs the REAL sw/os/usb driver against the
 * REAL rtl/usb gateware.
 *
 * Up to this point the two halves of the USB work had each been tested
 * and had never met. The RTL was exercised by a behavioural device
 * model driving its Wishbone port from Verilog tasks; the driver was
 * exercised by the compiler. Everything between them -- whether the
 * driver's idea of the register map matches the hardware's, whether
 * the enumeration sequence is one the device model will actually
 * answer, whether "poll until not pending" terminates -- was untested
 * and would have been debugged on a board.
 *
 * -- how it works --
 *
 * The driver runs in a pthread. Its register accessors (z_usbh_rd and
 * friends, see sw/os/usb/usbh_hw.h) post a request and block. The
 * Verilog side calls $usbh_step once per bus-idle moment, which
 * releases the driver thread and blocks the SIMULATOR until the driver
 * either posts a request or says it has finished this step.
 *
 * Only one of the two ever runs at a time, so there is no race and no
 * need for the simulator to model concurrency it does not have.
 * Simulation time is frozen while the driver computes, which is
 * exactly right: the driver is code the CPU would have run between bus
 * cycles, and the CPU is not modelled here.
 *
 * The driver is compiled with -DZ_USBH_COSIM, which is the only
 * difference from the kernel build. Everything else -- the state
 * machine, the descriptor walk, the register bit layouts -- is the
 * same source the kernel links.
 */

#include <vpi_user.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define ST_COMPUTING 0   /* driver thread is running */
#define ST_REQUEST   1   /* driver wants a bus cycle */
#define ST_IDLE      2   /* driver finished a step, sim may advance */

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t to_driver = PTHREAD_COND_INITIALIZER;
static pthread_cond_t to_sim = PTHREAD_COND_INITIALIZER;

static int state = ST_IDLE;
static int go;                  /* sim has released the driver */

static uint32_t req_adr;
static uint32_t req_wdat;
static uint32_t req_rdat;
static int req_we;
static int req_bw;              /* byte-wide access */

static pthread_t thr;
static int started;

/* The kernel tick the driver uses for coarse timeouts. Advanced by the
 * Verilog side so that a driver state waiting on one actually
 * progresses -- without this, enumeration stalls forever in the 2 ms
 * settle after SET_ADDRESS and the failure looks like a dead device. */
volatile uint32_t z_kernel_ticks;

void z_usbh_init(void);
void z_usbh_poll(void);

/* -- mass storage requests --
 *
 * The driver thread normally runs one z_usbh_poll() per step. When the
 * harness arms a request with $usbh_msc_arm, the NEXT step runs that
 * instead: the same blocking call diskio_mux makes from FatFs, start
 * to finish, with every register access it makes serviced as a real
 * bus cycle. $usbh_msc_result then reports what it returned and how
 * the sector compared with the device model's medium. */
int z_usbh_msc_start(void);
int z_usbh_msc_read(uint32_t lba, uint8_t *dst, uint32_t count);
int z_usbh_msc_write(uint32_t lba, const uint8_t *src, uint32_t count);

#define MSC_OP_START 0
#define MSC_OP_READ  1
#define MSC_OP_WRITE 2      /* write a distinct pattern, then read back */

static int msc_pending, msc_op, msc_lba;
static int msc_rc, msc_bad, msc_first_bad;
static uint8_t msc_written[16];

/* rtl/tb/tb_usb_device.v's medium, byte for byte. */
static uint8_t msc_pattern(int lba, int i)
{
    int g = lba * 512 + i;
    if (msc_written[lba & 15]) return (uint8_t)(i * 5 + lba + 0x5a);
    if (lba == 0) {
        if (i == 0) return 0xeb;
        if (i == 1) return 0x3c;
        if (i == 2) return 0x90;
        if (i == 3) return 0x6d;
        if (i == 510) return 0x55;
        if (i == 511) return 0xaa;
    }
    return (uint8_t)((g * 7 + (g >> 9) * 13) & 0xff);
}

static void msc_run(void)
{
    static uint8_t buf[512];
    int i;

    msc_bad = 0;
    msc_first_bad = -1;

    if (msc_op == MSC_OP_START) {
        msc_rc = z_usbh_msc_start();
        return;
    }

    if (msc_op == MSC_OP_WRITE) {
        for (i = 0; i < 512; i++)
            buf[i] = (uint8_t)(i * 5 + msc_lba + 0x5a);
        msc_rc = z_usbh_msc_write((uint32_t)msc_lba, buf, 1);
        if (msc_rc != 0) return;
        msc_written[msc_lba & 15] = 1;
    }

    /* Poison first: a driver that returns success without filling
     * the buffer must not pass by leaving the previous sector there. */
    memset(buf, 0xa5, sizeof(buf));
    msc_rc = z_usbh_msc_read((uint32_t)msc_lba, buf, 1);
    if (msc_rc != 0) return;
    for (i = 0; i < 512; i++) {
        if (buf[i] != msc_pattern(msc_lba, i)) {
            if (msc_first_bad < 0) msc_first_bad = i;
            msc_bad++;
        }
    }
}

/* -- the driver side of the handshake -- */

static void bus_xfer(void)
{
    pthread_mutex_lock(&mtx);
    state = ST_REQUEST;
    pthread_cond_signal(&to_sim);
    while (state == ST_REQUEST)
        pthread_cond_wait(&to_driver, &mtx);
    pthread_mutex_unlock(&mtx);
}

uint32_t z_usbh_rd(uint32_t a)
{
    req_adr = a; req_we = 0; req_bw = 0;
    bus_xfer();
    return req_rdat;
}

void z_usbh_wr(uint32_t a, uint32_t v)
{
    req_adr = a; req_we = 1; req_bw = 0; req_wdat = v;
    bus_xfer();
}

uint8_t z_usbh_rb(uint32_t a)
{
    req_adr = a; req_we = 0; req_bw = 1;
    bus_xfer();
    return (uint8_t)req_rdat;
}

void z_usbh_wb(uint32_t a, uint8_t v)
{
    req_adr = a; req_we = 1; req_bw = 1; req_wdat = v;
    bus_xfer();
}

/* Stub for sw/common/zsoc.h's feature check. The bitstream under test
 * has the block by construction, so this is not where a missing
 * feature would be caught. */
int z_soc_has_feature2(uint32_t bit)
{
    (void)bit;
    return 1;
}

static void *driver_main(void *arg)
{
    (void)arg;

    pthread_mutex_lock(&mtx);
    while (!go) pthread_cond_wait(&to_driver, &mtx);
    pthread_mutex_unlock(&mtx);

    z_usbh_init();

    for (;;) {
        pthread_mutex_lock(&mtx);
        state = ST_IDLE;
        go = 0;
        pthread_cond_signal(&to_sim);
        while (!go) pthread_cond_wait(&to_driver, &mtx);
        state = ST_COMPUTING;
        pthread_mutex_unlock(&mtx);

        if (msc_pending) {
            msc_run();
            msc_pending = 0;
        } else {
            z_usbh_poll();
        }
    }

    return NULL;
}

/* -- the simulator side -- */

static void put_int(vpiHandle arg, int v)
{
    s_vpi_value val;
    val.format = vpiIntVal;
    val.value.integer = v;
    vpi_put_value(arg, &val, NULL, vpiNoDelay);
}

static int get_int(vpiHandle arg)
{
    s_vpi_value val;
    val.format = vpiIntVal;
    vpi_get_value(arg, &val);
    return val.value.integer;
}

/*
 * $usbh_step(valid, we, adr, wdat, bw)
 *
 * Releases the driver and blocks until it either wants a bus cycle
 * (valid=1, the request in the other arguments) or has finished this
 * step (valid=0). The simulator must not advance time between the two.
 */
static PLI_INT32 usbh_step_calltf(PLI_BYTE8 *ud)
{
    vpiHandle sys, argv, a[5];
    int i;
    (void)ud;

    sys = vpi_handle(vpiSysTfCall, NULL);
    argv = vpi_iterate(vpiArgument, sys);
    for (i = 0; i < 5; i++) a[i] = vpi_scan(argv);

    if (!started) {
        started = 1;
        pthread_create(&thr, NULL, driver_main, NULL);
    }

    pthread_mutex_lock(&mtx);
    go = 1;
    state = ST_COMPUTING;
    pthread_cond_signal(&to_driver);
    while (state == ST_COMPUTING)
        pthread_cond_wait(&to_sim, &mtx);

    if (state == ST_REQUEST) {
        put_int(a[0], 1);
        put_int(a[1], req_we);
        put_int(a[2], (int)req_adr);
        put_int(a[3], (int)req_wdat);
        put_int(a[4], req_bw);
    } else {
        put_int(a[0], 0);
        put_int(a[1], 0);
        put_int(a[2], 0);
        put_int(a[3], 0);
        put_int(a[4], 0);
    }
    pthread_mutex_unlock(&mtx);

    return 0;
}

/*
 * $usbh_done(rdat, valid, we, adr, wdat, bw)
 *
 * Completes the bus cycle and reports the NEXT request in the same
 * call. Reporting it separately would mean the Verilog side had to
 * advance time to ask, and a driver routine that does several accesses
 * in a row -- which every one of them does -- would get a clock edge
 * inserted in the middle of what the CPU would have executed with no
 * bus activity at all.
 */
static PLI_INT32 usbh_done_calltf(PLI_BYTE8 *ud)
{
    vpiHandle sys, argv, a[6];
    int i;
    (void)ud;

    sys = vpi_handle(vpiSysTfCall, NULL);
    argv = vpi_iterate(vpiArgument, sys);
    for (i = 0; i < 6; i++) a[i] = vpi_scan(argv);

    pthread_mutex_lock(&mtx);
    req_rdat = (uint32_t)get_int(a[0]);
    state = ST_COMPUTING;
    pthread_cond_signal(&to_driver);
    while (state == ST_COMPUTING)
        pthread_cond_wait(&to_sim, &mtx);

    if (state == ST_REQUEST) {
        put_int(a[1], 1);
        put_int(a[2], req_we);
        put_int(a[3], (int)req_adr);
        put_int(a[4], (int)req_wdat);
        put_int(a[5], req_bw);
    } else {
        put_int(a[1], 0);
        put_int(a[2], 0);
        put_int(a[3], 0);
        put_int(a[4], 0);
        put_int(a[5], 0);
    }
    pthread_mutex_unlock(&mtx);

    return 0;
}

/* $usbh_tick -- advance the driver's notion of kernel time. */
static PLI_INT32 usbh_tick_calltf(PLI_BYTE8 *ud)
{
    (void)ud;
    z_kernel_ticks++;
    return 0;
}

/* $usbh_msc_arm(op, lba) -- the next driver step runs this request. */
static PLI_INT32 usbh_msc_arm_calltf(PLI_BYTE8 *ud)
{
    vpiHandle sys, argv;
    (void)ud;

    sys = vpi_handle(vpiSysTfCall, NULL);
    argv = vpi_iterate(vpiArgument, sys);
    msc_op = get_int(vpi_scan(argv));
    msc_lba = get_int(vpi_scan(argv));
    vpi_free_object(argv);
    msc_pending = 1;
    return 0;
}

/* $usbh_msc_result(rc, bad, first_bad) -- what the last request did.
 * rc is the driver's return code; bad counts sector bytes that differ
 * from the model's medium, first_bad is the offset of the first. */
static PLI_INT32 usbh_msc_result_calltf(PLI_BYTE8 *ud)
{
    vpiHandle sys, argv;
    (void)ud;

    sys = vpi_handle(vpiSysTfCall, NULL);
    argv = vpi_iterate(vpiArgument, sys);
    put_int(vpi_scan(argv), msc_rc);
    put_int(vpi_scan(argv), msc_bad);
    put_int(vpi_scan(argv), msc_first_bad);
    vpi_free_object(argv);
    return 0;
}

static PLI_INT32 compiletf_any(PLI_BYTE8 *ud) { (void)ud; return 0; }

void usbh_register(void)
{
    s_vpi_systf_data t;

    memset(&t, 0, sizeof(t));
    t.type = vpiSysTask;
    t.tfname = "$usbh_step";
    t.calltf = usbh_step_calltf;
    t.compiletf = compiletf_any;
    vpi_register_systf(&t);

    memset(&t, 0, sizeof(t));
    t.type = vpiSysTask;
    t.tfname = "$usbh_done";
    t.calltf = usbh_done_calltf;
    t.compiletf = compiletf_any;
    vpi_register_systf(&t);

    memset(&t, 0, sizeof(t));
    t.type = vpiSysTask;
    t.tfname = "$usbh_tick";
    t.calltf = usbh_tick_calltf;
    t.compiletf = compiletf_any;
    vpi_register_systf(&t);

    memset(&t, 0, sizeof(t));
    t.type = vpiSysTask;
    t.tfname = "$usbh_msc_arm";
    t.calltf = usbh_msc_arm_calltf;
    t.compiletf = compiletf_any;
    vpi_register_systf(&t);

    memset(&t, 0, sizeof(t));
    t.type = vpiSysTask;
    t.tfname = "$usbh_msc_result";
    t.calltf = usbh_msc_result_calltf;
    t.compiletf = compiletf_any;
    vpi_register_systf(&t);
}

void (*vlog_startup_routines[])(void) = { usbh_register, 0 };
