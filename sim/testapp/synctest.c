#include <stdint.h>

#define reg_kernel (*(volatile uint32_t*)0x0000000c)

typedef uint32_t *(*z_kernel_ptr_t)(uint32_t, uint32_t *, uint32_t);

typedef struct {
	int32_t type;
	int32_t val; /* only using the int32 member of the union */
} z_obj_t;

enum { ZSYS_NONE=0, ZSYS_EXIT, ZSYS_UI_PRINT, ZSYS_UART_GETC, ZSYS_UART_PUTC,
       ZSYS_UART_RX_EMPTY, ZSYS_UART_TX_FULL };

static void uart_putc(char c) {
	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)reg_kernel;
	z_obj_t obj;
	obj.val = c;
	k(ZSYS_UART_PUTC, (uint32_t *)&obj, 0);
}

static void uart_puts(const char *s) {
	while (*s) uart_putc(*s++);
}

int main(void) {
	uart_puts("HELLO FROM SYSCALL GATE\r\n");

	z_kernel_ptr_t k = (z_kernel_ptr_t)(uintptr_t)reg_kernel;
	z_obj_t obj;
	k(ZSYS_UART_PUTC, (uint32_t *)&obj, 0); /* not used, just exercised above */

	uart_puts("DONE\r\n");

	k(ZSYS_EXIT, (uint32_t *)0, 0);

	for (;;) ; /* should never get here */
}
