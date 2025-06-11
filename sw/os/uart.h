#ifndef Z_UART_H
#define Z_UART_H

void z_uart_init(void);
void z_uart_irq(void);

void k_uart_putc(char c);
int16_t k_uart_getc(void);
bool k_uart_rx_empty(void);
bool k_uart_tx_full(void);

// --

z_obj_t *z_uart_getc(z_obj_t *obj);
z_obj_t *z_uart_putc(z_obj_t *obj);
z_obj_t *z_uart_rx_empty(z_obj_t *obj);
z_obj_t *z_uart_tx_full(z_obj_t *obj);

#endif
