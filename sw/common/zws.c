/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Server-side WebSocket (RFC 6455). See zws.h.
 */

#include <string.h>

#include "zsha1.h"
#include "zws.h"

// -- handshake --

int z_ws_accept(char out[Z_WS_ACCEPT_LEN + 1], const char *key, size_t keylen)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	z_sha1_ctx c;
	uint8_t d[Z_SHA1_DIGEST];

	if (keylen != Z_WS_KEY_LEN)
		return -1;
	z_sha1_init(&c);
	z_sha1_update(&c, key, (uint32_t)keylen);
	z_sha1_update(&c, guid, sizeof(guid) - 1);
	z_sha1_final(&c, d);
	z_base64_encode(out, d, sizeof d);
	return 0;
}

size_t z_ws_response(char *out, size_t cap, const char *key, size_t keylen)
{
	static const char head[] = "HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: ";
	size_t n = sizeof(head) - 1;

	if (cap < Z_WS_RESPONSE_MAX)
		return 0;
	memcpy(out, head, n);
	if (z_ws_accept(out + n, key, keylen))
		return 0;
	n += Z_WS_ACCEPT_LEN;
	memcpy(out + n, "\r\n\r\n", 5);	// and the NUL
	return n + 4;
}

// -- sending --

size_t z_ws_header(uint8_t out[Z_WS_HEADER_MAX], uint8_t opcode, uint64_t len)
{
	out[0] = (uint8_t)(0x80 | (opcode & 0x0f));
	if (len < 126) {
		out[1] = (uint8_t)len;
		return 2;
	}
	if (len < 65536) {
		out[1] = 126;
		out[2] = (uint8_t)(len >> 8);
		out[3] = (uint8_t)len;
		return 4;
	}
	out[1] = 127;
	for (int i = 0; i < 8; i++)
		out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
	return 10;
}

size_t z_ws_control(uint8_t out[Z_WS_CONTROL_MAX], uint8_t opcode,
	const void *payload, size_t len)
{
	if (len > 125)
		len = 125;
	z_ws_header(out, opcode, len);
	if (len)
		memcpy(out + 2, payload, len);
	return 2 + len;
}

size_t z_ws_close(uint8_t out[Z_WS_CONTROL_MAX], uint16_t code)
{
	uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };

	return z_ws_control(out, Z_WS_CLOSE, p, 2);
}

// -- receiving --

enum {
	ST_B0,		// first header byte
	ST_B1,		// mask bit and 7-bit length
	ST_EXT,		// 16- or 64-bit length
	ST_MASK,	// four mask bytes
	ST_DATA,	// payload of a data frame, delivered as it comes
	ST_CTL,		// payload of a control frame, held until whole
	ST_DEAD		// after an error
};

void z_ws_parser_init(z_ws_parser *p, uint32_t max_payload)
{
	memset(p, 0, sizeof(*p));
	p->st = ST_B0;
	p->max_payload = max_payload;
}

static size_t fail(z_ws_parser *p, z_ws_event *ev, uint16_t code, size_t used)
{
	p->st = ST_DEAD;
	ev->type = Z_WS_EV_ERROR;
	ev->code = code;
	return used;
}

static void frame_end(z_ws_parser *p, z_ws_event *ev)
{
	uint32_t n = p->ctl_len;

	p->st = ST_B0;
	switch (p->op) {
	case Z_WS_PING:
		ev->type = Z_WS_EV_PING;
		ev->data = p->ctl;
		ev->len = n;
		break;
	case Z_WS_PONG:
		ev->type = Z_WS_EV_PONG;
		ev->data = p->ctl;
		ev->len = n;
		break;
	case Z_WS_CLOSE:
		if (n == 1) {		// a status code is two bytes
			p->st = ST_DEAD;
			ev->type = Z_WS_EV_ERROR;
			ev->code = Z_WS_CLOSE_PROTOCOL;
			break;
		}
		ev->type = Z_WS_EV_CLOSE;
		ev->code = n ? (uint16_t)(p->ctl[0] << 8 | p->ctl[1]) : 1005;
		ev->data = p->ctl + (n ? 2 : 0);
		ev->len = n ? n - 2 : 0;
		break;
	}
}

// The header is complete. Either a frame with payload to come (and
// ev left as NONE), or one with no payload at all, delivered now.
static void frame_start(z_ws_parser *p, z_ws_event *ev)
{
	p->mask_i = 0;
	p->ctl_len = 0;
	if (p->op >= Z_WS_CLOSE) {
		if (p->remain) {
			p->st = ST_CTL;
			return;
		}
		frame_end(p, ev);
		return;
	}
	if (!p->remain) {
		// an empty data frame still ends (or continues) a message
		ev->type = Z_WS_EV_DATA;
		ev->opcode = p->op == Z_WS_CONT ? p->msg_op : p->op;
		ev->fin = p->fin;
		ev->data = p->ctl;
		ev->len = 0;
		if (p->fin)
			p->msg_op = 0;
		p->st = ST_B0;
		return;
	}
	p->st = ST_DATA;
}

size_t z_ws_feed(z_ws_parser *p, uint8_t *in, size_t n, z_ws_event *ev)
{
	size_t i = 0;

	memset(ev, 0, sizeof(*ev));
	ev->type = Z_WS_EV_NONE;

	while (i < n && ev->type == Z_WS_EV_NONE) {
		uint8_t b = in[i];

		switch (p->st) {
		case ST_DEAD:
			return n;
		case ST_B0:
			if (b & 0x70)			// reserved bits: no extension was offered
				return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i + 1);
			p->fin = b >> 7;
			p->op = b & 0x0f;
			if (p->op >= Z_WS_CLOSE) {
				if (p->op > Z_WS_PONG || !p->fin)
					return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i + 1);
			} else if (p->op == Z_WS_CONT) {
				if (!p->msg_op)
					return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i + 1);
			} else if (p->op > Z_WS_BINARY || p->msg_op) {
				return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i + 1);
			}
			if (p->op == Z_WS_TEXT || p->op == Z_WS_BINARY)
				p->msg_op = p->op;
			p->st = ST_B1;
			i++;
			break;
		case ST_B1:
			if (!(b & 0x80))		// a client must mask
				return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i + 1);
			b &= 0x7f;
			i++;
			p->remain = b;
			if (b >= 126) {
				if (p->op >= Z_WS_CLOSE)
					return fail(p, ev, Z_WS_CLOSE_PROTOCOL, i);
				p->ext = b == 126 ? 2 : 8;
				p->remain = 0;
				p->st = ST_EXT;
			} else {
				p->ext = 0;
				p->st = ST_MASK;
			}
			p->mask_i = 0;
			break;
		case ST_EXT:
			// Network order. The top four bytes of a 64-bit length
			// must be zero: anything longer than 32 bits is too big
			// whatever max_payload is.
			if (p->ext > 4 && b)
				return fail(p, ev, Z_WS_CLOSE_TOO_BIG, i + 1);
			if (p->ext <= 4)
				p->remain = p->remain << 8 | b;
			i++;
			if (--p->ext == 0) {
				if (p->remain > p->max_payload)
					return fail(p, ev, Z_WS_CLOSE_TOO_BIG, i);
				p->st = ST_MASK;
			}
			break;
		case ST_MASK:
			p->mask[p->mask_i++] = b;
			i++;
			if (p->mask_i == 4) {
				if (p->op < Z_WS_CLOSE && p->remain > p->max_payload)
					return fail(p, ev, Z_WS_CLOSE_TOO_BIG, i);
				frame_start(p, ev);
			}
			break;
		case ST_CTL:
			p->ctl[p->ctl_len] = b ^ p->mask[p->mask_i++ & 3];
			p->ctl_len++;
			i++;
			if (--p->remain == 0)
				frame_end(p, ev);
			break;
		case ST_DATA: {
			uint32_t take = p->remain;
			uint8_t *d = in + i;

			if (take > n - i)
				take = (uint32_t)(n - i);
			for (uint32_t k = 0; k < take; k++)
				d[k] ^= p->mask[p->mask_i++ & 3];
			p->remain -= take;
			i += take;
			ev->type = Z_WS_EV_DATA;
			ev->opcode = p->op == Z_WS_CONT ? p->msg_op : p->op;
			ev->fin = p->remain == 0 && p->fin;
			ev->data = d;
			ev->len = take;
			if (!p->remain) {
				if (p->fin)
					p->msg_op = 0;
				p->st = ST_B0;
			}
			break;
		}
		}
	}
	return i;
}
