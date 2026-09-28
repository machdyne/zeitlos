/*
 * Host test for mesh0 (mesh_svc.c): the real service against a scripted
 * kernel -- what it sends is caught; messages from clients and from
 * serial are made here. docs/mesh_app.md, "mesh0".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../mesh_svc.h"
#include "../mesh_pb.h"
#include "zport.h"
#include "zobj.h"
#include "zproc.h"

static int run, failed;
#define CHECK(c, what) do { run++; if (!(c)) { failed++; printf("FAIL %d: %s\n", __LINE__, what); } } while (0)

#define CLIENT 70
#define OTHER  71
#define SERIAL 30

// -- what the service sends --
static struct { uint32_t to, subject, tag; uint8_t d[400]; uint32_t n; } out[64];
static int nout;
static uint32_t dead_pid, ticks = 1000;
static z_obj_t k_ok, k_fail;

static uint32_t *k_syscall(uint32_t id, uint32_t *args, uint32_t b) {
	(void)b;
	switch (id) {
	case Z_SYS_MSG_SEND: {
		z_msg_t *m = (z_msg_t *)args;
		if (nout < 64) {
			out[nout].to = m->to; out[nout].subject = m->subject; out[nout].tag = m->tag; out[nout].n = 0;
			if (m->obj.type == Z_BLOB) { z_blob_t *bl = m->obj.val.ptr; memcpy(out[nout].d, bl->data, bl->len); out[nout].n = bl->len; }
			if (m->obj.type == Z_UINT32) { memcpy(out[nout].d, &m->obj.val.uint32, 4); out[nout].n = 4; }
			nout++;
		}
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_PID_REGISTER: {
		z_obj_t *o = (z_obj_t *)args;
		o->type = Z_STR; o->val.str = "mesh0";
		return (uint32_t *)&k_ok;
	}
	case Z_SYS_UPTIME:
		((z_obj_t *)args)->type = Z_UINT32; ((z_obj_t *)args)->val.uint32 = ticks;
		return (uint32_t *)&k_ok;
	case Z_SYS_PROC_STATUS: {
		z_proc_status_args_t *a = (z_proc_status_args_t *)args;
		a->state = a->pid == dead_pid ? Z_PROC_STATE_EXITED : Z_PROC_STATE_RUNNING;
		return (uint32_t *)&k_ok;
	}
	}
	return (uint32_t *)&k_ok;
}

static bool k_install(void) {
	if ((uintptr_t)(void *)k_syscall > 0xFFFFFFFFu) return false;
	if (mmap((void *)0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED)
		return false;
	k_ok.type = Z_UINT32; k_ok.val.uint32 = Z_OK;
	k_fail.type = Z_UINT32; k_fail.val.uint32 = Z_FAIL;
	*(volatile uint32_t *)0x0000000c = (uint32_t)(uintptr_t)k_syscall;
	return true;
}

// -- messages to the service --
static z_blob_t blob;
static uint8_t blob_d[400];
static z_msg_t msg(uint32_t from, uint32_t subject, uint32_t tag, const void *d, uint32_t n) {
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	m.from = from; m.subject = subject; m.tag = tag;
	if (d) {
		memcpy(blob_d, d, n); blob.data = blob_d; blob.len = n;
		m.obj.type = Z_BLOB; m.obj.val.ptr = &blob;
	} else m.obj.type = Z_NONE;
	return m;
}

// The last record sent to `to`, as DATA; its length, or -1.
static int last_to(uint32_t to, uint8_t *d) {
	for (int i = nout - 1; i >= 0; i--)
		if (out[i].to == to && out[i].subject == Z_PORT_DATA) { memcpy(d, out[i].d, out[i].n); return (int)out[i].n; }
	return -1;
}

// -- the session: its sends caught --
static uint8_t wire[1024];
static uint32_t wire_n;
static int t_send(void *ctx, const uint8_t *p, uint32_t n) { (void)ctx; memcpy(wire + wire_n, p, n); wire_n += n; return 0; }

int main(void) {
	static mesh_model_t M;
	static mesh_session_t S;
	uint8_t d[400];
	if (!k_install()) { printf("mesh0 test: skipped (cannot map page 0)\n"); return 77; }
	mesh_model_init(&M);
	mesh_session_init(&S, &M);
	S.send = t_send;

	CHECK(mesh_svc_start(NULL), "mesh0 registered");
	// a client connects: told at once the radio is down
	z_msg_t m = msg(CLIENT, Z_PORT_CONNECT, 5, NULL, 0);
	CHECK(mesh_svc_msg(&m, &S), "a CONNECT: the service's");
	uint32_t cid = 0;
	for (int i = 0; i < nout; i++) if (out[i].to == CLIENT && out[i].subject == Z_PORT_CONNECTED) memcpy(&cid, out[i].d, 4);
	CHECK(cid != 0 && last_to(CLIENT, d) == 1 && d[0] == 'D', "accepted, and told: the radio is down");

	// the radio comes up: told, with our node number
	S.state = MESH_ST_LIVE;
	M.my_num = 0xa1b2c3d4u;
	mesh_svc_tick(&S);
	CHECK(last_to(CLIENT, d) == 5 && d[0] == 'U' && d[1] == 0xd4 && d[4] == 0xa1, "the radio up: 'U', our node number");

	// listening on 300; a packet on 300 comes, one on 301 does not
	uint8_t L[3] = { 'L', 300 & 0xff, 300 >> 8 };
	m = msg(CLIENT, Z_PORT_DATA, cid, L, 3);
	CHECK(mesh_svc_msg(&m, &S), "'L' 300: taken");
	int before = nout;
	mesh_ev_t ev;
	memset(&ev, 0, sizeof(ev));
	uint8_t payload[233];
	for (int i = 0; i < 233; i++) payload[i] = (uint8_t)(i * 3);
	ev.kind = MESH_EV_DATA; ev.from = 0x11223344u; ev.to = 0xffffffffu; ev.channel = 1; ev.port = 300;
	ev.data = payload; ev.data_len = 233;
	mesh_svc_event(&ev);
	CHECK(last_to(CLIENT, d) == 12 + 233 && d[0] == 'R' && d[1] == 0x44 && d[9] == 1 && d[10] == 44 && d[11] == 1 &&
		!memcmp(d + 12, payload, 233), "a packet on 300: an 'R', its sender, channel, port and every byte");
	before = nout;
	ev.port = 301;
	mesh_svc_event(&ev);
	CHECK(nout == before, "one on 301: not to a client listening on 300");

	// sending: to the node; refused, saying why
	uint8_t Srec[8 + 234];
	Srec[0] = 'S'; memset(Srec + 1, 0xff, 4); Srec[5] = 1; Srec[6] = 300 & 0xff; Srec[7] = 300 >> 8;
	memcpy(Srec + 8, payload, 100);
	wire_n = 0;
	m = msg(CLIENT, Z_PORT_DATA, cid, Srec, 108);
	mesh_svc_msg(&m, &S);
	CHECK(wire_n > 100, "'S' while up: handed to the node");
	Srec[6] = 100; Srec[7] = 0;
	m = msg(CLIENT, Z_PORT_DATA, cid, Srec, 108);
	mesh_svc_msg(&m, &S);
	CHECK(last_to(CLIENT, d) > 1 && d[0] == 'E' && strstr((char *)d + 1, "256"), "'S' on port 100: refused -- Meshtastic's own");
	Srec[6] = 300 & 0xff; Srec[7] = 300 >> 8;
	m = msg(CLIENT, Z_PORT_DATA, cid, Srec, 8 + 234);
	mesh_svc_msg(&m, &S);
	CHECK(last_to(CLIENT, d) > 1 && d[0] == 'E' && strstr((char *)d + 1, "233"), "234 bytes: refused -- more than a packet");
	S.state = MESH_ST_DOWN;
	m = msg(CLIENT, Z_PORT_DATA, cid, Srec, 108);
	mesh_svc_msg(&m, &S);
	CHECK(last_to(CLIENT, d) > 1 && d[0] == 'E' && strstr((char *)d + 1, "not up"), "the radio down: refused");
	mesh_svc_tick(&S);
	CHECK(last_to(CLIENT, d) == 1 && d[0] == 'D', "and told: 'D'");

	// serial's messages are not the service's -- the radio's data still reaches mesh
	m = msg(SERIAL, Z_PORT_DATA, 1, "xx", 2);
	CHECK(!mesh_svc_msg(&m, &S), "DATA from serial: left for mesh");
	m = msg(OTHER, Z_PORT_DATA, cid, "xx", 2);
	CHECK(!mesh_svc_msg(&m, &S), "DATA with a client's id, from someone else: not taken for it");

	// a client that dies is forgotten: its slot free again
	dead_pid = CLIENT;
	ticks += 5000;
	mesh_svc_tick(&S);
	before = nout;
	mesh_svc_event(&ev);
	ev.port = 300;
	mesh_svc_event(&ev);
	CHECK(nout == before, "a client that died: forgotten -- nothing more sent to it");

	printf("mesh0 test: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;
}
