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

        z_usbh_poll();
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
}

void (*vlog_startup_routines[])(void) = { usbh_register, 0 };
