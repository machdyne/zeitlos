#ifndef Z_UART_H
#define Z_UART_H

void z_uart_init(void);
void z_uart_irq(void);

void k_uart_putc(char c);
int16_t k_uart_getc(void);
bool k_uart_rx_empty(void);
bool k_uart_tx_full(void);

// Block the current process until a byte is in the RX ring (UART
// RX IRQ unblocks). Used by the kernel shell so pid 0 is not
// RUNNABLE while waiting at the prompt.
void k_uart_wait_rx(void);

// --

z_obj_t *z_uart_getc(z_obj_t *obj);
z_obj_t *z_uart_putc(z_obj_t *obj);
z_obj_t *z_uart_rx_empty(z_obj_t *obj);
z_obj_t *z_uart_tx_full(z_obj_t *obj);

// -- console log (sw/common/zconsole.h, docs/console.md) --
// Record one byte of console output. k_uart_putc() does this itself;
// kprint() and anything else that writes UART0 directly must call it.
void k_klog_putc(uint8_t c);
z_obj_t *k_klog_read(z_obj_t *args);
z_obj_t *k_console_input(z_obj_t *args);

// Interrupt context (and a panic): printing drains the UART by polling
// instead of blocking the current process. See sw/os/uart.c.
extern volatile bool k_uart_polled;
// For a panic: send what is still queued, then draw the end of the
// console log on the screen (docs/console.md, docs/mpu.md).
void k_uart_flush(void);
void k_klog_panic_screen(void);

#endif
