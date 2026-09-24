#ifndef Z_HID_H
#define Z_HID_H

void z_hid_init(void);
void z_hid_irq0(void);
void z_hid_irq1(void);

#define HID_FIFO_SIZE 32

int32_t k_hid_read_key(void);

// ring totals, for sh.c's `ic` -- see hid.c's hid_push()
void k_hid_stats(uint32_t *pushed, uint32_t *dropped);

// --

z_obj_t *z_hid_read_key(z_obj_t *obj);
z_obj_t *k_hid_ptr_subscribe(z_obj_t *obj);
z_obj_t *k_hid_inject(z_obj_t *obj);
z_obj_t *k_kbd_layout(z_obj_t *args);	// Z_SYS_KBD_LAYOUT

void k_hid_wake_subscriber(void);

#endif
