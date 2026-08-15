#ifndef ZNET_H
#define ZNET_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Networking app protocol -- shared between the net app
 * (sw/apps/net) and any process that wants to use it (the shell's
 * tget/tput commands, sw/os/sh.c, or any other app later). See
 * docs/networking.md.
 *
 * Both directions of a TFTP transfer move through sw/common/zstream.h
 * as a stream now, not a single message carrying the whole file --
 * see tftp.h for why. That changes how GET and PUT are each kicked
 * off:
 *
 * GET: net.c is the data source, so use zstream_open() (see
 * zstream.h) directly against Z_PID_NET, with a
 * Z_MAP{"ip":Z_UINT32, "filename":Z_STR} payload. net.c streams the
 * received data back as the reply stream -- Z_STREAM_EOF marks
 * successful completion, Z_STREAM_ERROR carries a failure message.
 * There's currently only one thing an OPEN to net.c can mean, so no
 * separate subject/discriminator is needed; if net.c gains other
 * producer roles later, an "op" key in the payload can disambiguate
 * then.
 *
 * PUT: the direction is reversed (net.c needs to pull data FROM the
 * requester, not push it), so this can't reuse zstream_open() the
 * same way -- Z_NET_TFTP_PUT below just tells net.c to start a PUT
 * and gives it enough to open its OWN stream back to the requester,
 * which is the actual data channel. The requester needs to be ready
 * to act as a zstream producer (respond to net's Z_STREAM_OPEN)
 * essentially as soon as it sends this.
 */

// the networking app's well-known pid, same convention as Z_PID_WM
// (zwm.h): sh.c runs as pid 0, so starting wm then net in that order
// (`run wm`, `run net`) reserves pid 1 for wm and pid 2 for net. The
// shell's `init` command does this without needing wm at all, for
// testing net in isolation -- see docs/networking.md. There's no
// dynamic discovery yet (see docs/messaging.md), so this is a hard
// assumption until a real registry exists.
#define Z_PID_NET   2

// -- message subjects --

// requester -> net: Z_MAP{"ip":Z_UINT32, "filename":Z_STR}. net opens
// a stream back to the requester (see zstream.h) to pull the file's
// bytes, then forwards each chunk to the TFTP server as it arrives.
#define Z_NET_TFTP_PUT         302

// net -> requester, reply to Z_NET_TFTP_PUT (same tag): Z_MAP with
// "ok" (Z_UINT32, 0 or 1). If ok, nothing else. If not ok, "error"
// (Z_STR) holds a message. Sent once the whole transfer -- including
// the remote server's handling of the final block -- completes, not
// when the requester finishes producing chunks (those two can finish
// at different times).
#define Z_NET_TFTP_PUT_REPLY   303

#endif
