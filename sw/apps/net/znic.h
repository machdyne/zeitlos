#ifndef ZNIC_H
#define ZNIC_H

/*
 * ZNIC -- UART framing between Zeitlos (esp32link.c) and the ESP32
 * firmware (esp32/zeitlos-nic). See docs/esp32-net.md.
 *
 *   0x7E 0x5A | ver:u8 | type:u8 | n:u16le | payload[n] | crc16-ccitt
 *
 * CRC is over ver..payload, init 0xFFFF, poly 0x1021, xorout 0.
 */

#include <stdint.h>

#define ZNIC_SYNC0        0x7E
#define ZNIC_SYNC1        0x5A
#define ZNIC_VER          1
#define ZNIC_MAX_PAYLOAD  1518

#define ZNIC_DATA      0x01
#define ZNIC_NOP       0x02
#define ZNIC_RX_POLL   0x03
#define ZNIC_HELLO     0x10
#define ZNIC_LINK      0x11
#define ZNIC_STA       0x20
#define ZNIC_STA_ACK   0x21
#define ZNIC_DATA_ACK  0x22

#define ZNIC_STA_OK    0

static inline uint16_t znic_crc16(const uint8_t *p, uint32_t n)
{
	uint16_t crc = 0xFFFF;
	while (n--) {
		crc ^= (uint16_t)(*p++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
			                     : (uint16_t)(crc << 1);
	}
	return crc;
}

#endif
