#ifndef ZNIC_FW_H
#define ZNIC_FW_H

#include <stdint.h>
#include <stddef.h>

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
#define ZNIC_LOG       0x30	/* ESP32 -> Z: one ESP_LOG line (text) */
#define ZNIC_STA_OK    0

typedef struct {
	uint8_t type;
	uint16_t len;
	uint8_t payload[ZNIC_MAX_PAYLOAD];
} znic_msg_t;

void znic_init(void);
int  znic_send(uint8_t type, const uint8_t *payload, uint16_t n);
int  znic_recv(znic_msg_t *msg, int timeout_ms);
uint16_t znic_crc16(const uint8_t *p, uint32_t n);
uint16_t znic_crc16_update(uint16_t crc, const uint8_t *p, uint32_t n);
/* Outbound control queue. Everything ESP32 -> Zeitlos that is not a
 * direct reply (HELLO, LINK, LOG) waits here and goes out one frame
 * per RX_POLL: Zeitlos reads UART1 by polling a 16-byte FIFO from a
 * time-sliced process, so it must ask for every frame. */
#define ZNIC_CTL_MAX   200
typedef struct {
	uint8_t type;
	uint16_t len;
	uint8_t payload[ZNIC_CTL_MAX];
} znic_ctl_t;
void znic_ctl_init(void);
int  znic_ctl_push(uint8_t type, const uint8_t *payload, uint16_t n);	/* 0 ok, -1 full */
int  znic_ctl_pop(znic_ctl_t *out);					/* 1 got one, 0 empty */
/* mirror ESP_LOG* output as ZNIC_LOG frames on the control queue (UART0 keeps it too) */
void znic_log_install(void);
/* once Zeitlos is talking, stop mirroring to UART0 (it is not wired to
 * anything under the SOC bitstream and costs ~8 ms per line) */
void znic_set_quiet_uart0(int quiet);
unsigned znic_ctl_depth(void);

#endif
