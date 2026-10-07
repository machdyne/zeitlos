/*
 * basic -- a host stand-in for basic_app.c: what the interpreter and the
 * extensions ask of the system, for tests/test_basic.c. Output goes to
 * the screen and, as text, to host_text. Files are in host_dir.
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "../../../ext/basic/basic.h"
#include "../bplat.h"

bscreen_t host_screen;
char host_text[65536];
int host_text_len;
bool host_full, host_full_available = true;
int host_syncs;
uint8_t host_keys[16];
int host_nkeys;
const char *host_dir = "/tmp/basic_host_files";

/* ---- the system (basic.h) ---- */

void hw_putc(char c) {
    bs_putc(&host_screen, (uint8_t)c);
    if (c != '\r' && host_text_len < (int)sizeof(host_text) - 1) {
        host_text[host_text_len++] = c;
        host_text[host_text_len] = 0;
    }
}

int hw_break(void) { return 0; }
void hw_delay_ms(uint16_t ms) { (void)ms; }

/* no pins: unused is fine, anything else is not supported */
int hw_pin_mode(uint8_t pin, uint8_t mode) {
    (void)pin;
    return mode == PM_NONE ? 0 : HW_ERR_UNSUPPORTED;
}
void hw_pin_write(uint8_t pin, uint8_t level) { (void)pin; (void)level; }
uint8_t hw_pin_read(uint8_t pin) { (void)pin; return 0; }
int16_t hw_adc(uint8_t pin) { (void)pin; return -1; }
void hw_led(uint8_t on) { (void)on; }
int hw_i2c(uint8_t addr, const uint8_t *w, uint8_t wn, uint8_t *r, uint8_t rn) {
    (void)addr; (void)w; (void)wn; (void)r; (void)rn;
    return -1;
}

static FILE *file;
static char file_path[300], file_tmp[300];
static uint8_t file_mode;

int hw_fopen(const char *name, uint8_t mode) {
    snprintf(file_path, sizeof(file_path), "%s/%s", host_dir, name);
    file_mode = mode;
    if (mode == FS_READ) file = fopen(file_path, "rb");
    else if (mode == FS_APPEND) file = fopen(file_path, "ab");
    else {
        snprintf(file_tmp, sizeof(file_tmp), "%s/_SAVING.TMP", host_dir);
        file = fopen(file_tmp, "wb");
    }
    return file ? FS_OK : mode == FS_READ ? FS_ERR_NOT_FOUND : FS_ERR_IO;
}

int hw_fread(uint8_t *buf, uint16_t len) { return (int)fread(buf, 1, len, file); }
/* FS_OK or an error -- not a byte count, unlike hw_fread */
int hw_fwrite(const uint8_t *buf, uint16_t len) {
    return fwrite(buf, 1, len, file) == len ? FS_OK : FS_ERR_IO;
}

int hw_fclose(void) {
    fclose(file);
    file = 0;
    if (file_mode == FS_WRITE) rename(file_tmp, file_path);
    return FS_OK;
}

void hw_fabort(void) {
    if (file) fclose(file);
    file = 0;
    if (file_mode == FS_WRITE) remove(file_tmp);
}

int hw_fdelete(const char *name) {
    char p[300];
    snprintf(p, sizeof(p), "%s/%s", host_dir, name);
    return remove(p) ? FS_ERR_NOT_FOUND : FS_OK;
}

int hw_fdir(fs_dir_cb cb) {
    DIR *d = opendir(host_dir);
    struct dirent *e;
    if (!d) return FS_OK;
    while ((e = readdir(d))) if (e->d_name[0] != '.' && e->d_name[0] != '_') cb(e->d_name, 0);
    closedir(d);
    return FS_OK;
}

int hw_fformat(void) { return HW_ERR_UNSUPPORTED; }

/* ---- the app (bplat.h) ---- */

bscreen_t *bp_screen(void) { return &host_screen; }

bool bp_full_screen(bool on) {
    if (!host_full_available) return false;
    host_full = on;
    return true;
}

uint8_t bp_key(void) {
    if (!host_nkeys) return 0;
    uint8_t k = host_keys[0];
    memmove(host_keys, host_keys + 1, (size_t)--host_nkeys);
    return k;
}

void bp_sync(void) { host_syncs++; }
