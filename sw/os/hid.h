#ifndef Z_HID_H
#define Z_HID_H

void z_hid_init(void);
void z_hid_irq0(void);
void z_hid_irq1(void);

int32_t k_hid_read_key(void);

// --

z_obj_t *z_hid_read_key(z_obj_t *obj);

#endif
