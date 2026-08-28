#ifndef TFTPD_H
#define TFTPD_H
/* TFTP server on the gateway (192.168.4.1, udp/69), own task with UDP
 * sockets so a request can block (HTTP): serves synthetic files
 * (test.bin 32 KiB, big.bin 256 KiB, byte i = (i*7+3) ^ (i>>8)), any
 * "http://..." or "https://..." name (fetched with esp_http_client and
 * streamed block by block, redirects followed), and counts uploads. */
void tftpd_start(void);
#endif
