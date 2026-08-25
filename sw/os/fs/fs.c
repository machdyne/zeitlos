#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs/ff.h"
#include "fs.h"
#include "../../common/zexec.h"

FATFS sdvol0;

int fs_load(uint32_t dst, char *path) {

	FIL f;
	FRESULT res;
	FSIZE_t sz;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);

	if (res != FR_OK)
		return 1;

	sz = f_size(&f);

	// printf("loading %li bytes ...\n", sz);

	int blks = sz / 1024;

	for (int i = 0; i < blks; i++) {
		res = f_read(&f, buf, 1024, &br);
		memcpy(dst_ptr, &buf, 1024);
		dst_ptr += 1024;
		if (res != FR_OK) return 1;
	}

	res = f_read(&f, buf, sz - (blks * 1024), &br);
	memcpy(dst_ptr, &buf, sz - (blks * 1024));
	if (res != FR_OK) return 1;

	f_close(&f);

	return 0;

}

void *fs_mallocfile(char *path) {

	FIL f;
	FRESULT res;
	FSIZE_t sz;
	UINT br;

	void *buf;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);

	if (res != FR_OK)
		return NULL;

	sz = f_size(&f);

	buf = malloc(sz);
	
	res = f_read(&f, buf, sz, &br);
	if (res != FR_OK) return NULL;

	f_close(&f);

	return buf;

}

int fs_touch(char *path) {

	FIL f;
	FRESULT res;

	res = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);

	if (res == FR_OK) {
		f_close(&f);
		printf("file touched.\n");
		return 0;
	}

	printf("write failed; error code: %i\n", res);
	return 1;

}

int fs_mount(void)
{
	FRESULT res;
	res = f_mount(&sdvol0, "", 0);
	if (res == FR_OK)
		return 0;
	else
		return 1;
}

int fs_format(void) {

	FRESULT res;
	BYTE work[FF_MAX_SS];

	printf("formating ...\n");

	res = f_mkfs("", 0, work, sizeof work);

	if (res == FR_OK) {
		printf("format succeeded.\n");
		return 0;
	}

	printf("write failed; error code: %i\n", res);
	return 1;

}

uint32_t fs_total(void) {
	FATFS *fs;
	FRESULT res;
	DWORD fre_clust;

	res = f_getfree("", &fre_clust, &fs);

	if (res == FR_OK) {
		return ((fs->n_fatent - 2) * fs->csize) / 2;
	}

	return 0;
}

uint32_t fs_free(void) {
	FATFS *fs;
	FRESULT res;
	DWORD fre_clust;

	res = f_getfree("", &fre_clust, &fs);

	if (res == FR_OK) {
		return (fre_clust * fs->csize) / 2;
	}

	return 0;
}

uint32_t fs_size(char *path) {
	FIL f;
	FRESULT res;
	FSIZE_t fs = 0;
	res = f_open(&f, path, FA_WRITE | FA_OPEN_EXISTING);
	if (res == FR_OK) {
		fs = f_size(&f);
	}
	f_close(&f);
	return(fs);
}

int fs_write_file(char *path, char *buf, uint32_t len) {

	FIL f;
	FRESULT res;
	UINT bw;

	res = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);

	if (res == FR_OK) {

		f_write(&f, buf, len, &bw);
		res = f_close(&f);

		return bw;

	}

	printf("write failed; error code: %i\n", res);
	return 0;

}

// -- chunked (streaming) read/write -- see fs.h --

int fs_open_write(FIL *f, char *path) {
	FRESULT res = f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS);
	if (res != FR_OK) {
		printf("fs_open_write: failed; error code: %i\n", res);
		return 0;
	}
	return 1;
}

int fs_write_chunk(FIL *f, const void *buf, uint32_t len) {
	UINT bw;
	FRESULT res = f_write(f, buf, len, &bw);
	if (res != FR_OK) {
		printf("fs_write_chunk: failed; error code: %i\n", res);
		return -1;
	}
	return (int)bw;
}

int fs_close_write(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

int fs_open_read(FIL *f, char *path) {
	FRESULT res = f_open(f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) {
		printf("fs_open_read: failed; error code: %i\n", res);
		return 0;
	}
	return 1;
}

int32_t fs_read_chunk(FIL *f, void *buf, uint32_t maxlen) {
	UINT br;
	FRESULT res = f_read(f, buf, maxlen, &br);
	if (res != FR_OK) {
		printf("fs_read_chunk: failed; error code: %i\n", res);
		return -1;
	}
	return (int32_t)br;	// 0 means EOF (nothing left to read), matching f_read()'s own convention
}

int fs_close_read(FIL *f) {
	FRESULT res = f_close(f);
	return (res == FR_OK) ? 1 : 0;
}

int fs_mkdir(char *path) {

	FRESULT res;

	printf("making directory '%s' ...\n", path);

	res = f_mkdir(path);

	if (res != FR_OK) {
		printf("mkdir failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

int fs_unlink(char *path) {

	FRESULT res;

	printf("deleting '%s' ...\n", path);

	res = f_unlink(path);

	if (res != FR_OK) {
		printf("unlink failed; error code: %i\n", res);
		return 1;
	}

	return 0;

}

void fs_list_dir(char *path) {

	FRESULT res;
	DIR dir;
	UINT i;
	static FILINFO fno;

	res = f_opendir(&dir, path);

	if (res == FR_OK) {
		for (;;) {
			res = f_readdir(&dir, &fno);
			if (res != FR_OK || fno.fname[0] == 0) break;
			if (fno.fattrib & AM_DIR) {
				i = strlen(path);
				printf("%s\n", fno.fname);
				if (res != FR_OK) break;
					path[i] = 0;
			} else {
				printf("%s/%s\n", path, fno.fname);
			}
		}
		f_closedir(&dir);
	}

	return;

}

// -- Zeitlos executable format (sw/common/zexec.h) --
//
// fs_load() above loads a file verbatim and knows nothing about what
// is in it. These two understand the ZEXE header: bss is a number to
// be memset() rather than a region of zeros to be read off the card,
// which is where the load-time saving comes from.
//
// Split into inspect-then-load, not one call, because the caller needs
// the image size BEFORE it can allocate: k_proc_create() has to be
// handed data_size + bss_size, and only then is there a base address
// to load into.

// Reads and parses the header. Returns 0 on success. A file with no
// magic is not an error -- it reports as a legacy raw binary, which is
// exactly right for an old --pad-to image whose bss is already present
// as zeros (see z_exec_parse()).
int fs_exec_info(char *path, z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br = 0;
	uint8_t hdr[Z_EXEC_HEADER_SIZE];

	if (!path || !info) return 1;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return 1;

	uint32_t sz = (uint32_t)f_size(&f);

	res = f_read(&f, hdr, Z_EXEC_HEADER_SIZE, &br);
	f_close(&f);

	if (res != FR_OK) return 1;

	return z_exec_parse(hdr, (uint32_t)br, sz, info);

}

// Loads the data section to `dst` and zeroes the bss immediately after
// it. `info` must come from fs_exec_info() on the same file.
//
// The bss memset() is the entire point of the format: for `repl` that
// is ~110KB that used to be read from the SD card one 1KB block at a
// time and is now a single memset over RAM.
int fs_load_exec(uint32_t dst, char *path, const z_exec_info_t *info) {

	FIL f;
	FRESULT res;
	UINT br;

	char buf[1024];
	char *dst_ptr = (char *)dst;

	if (!path || !info) return 1;

	res = f_open(&f, path, FA_READ | FA_OPEN_EXISTING);
	if (res != FR_OK) return 1;

	// skip the header -- legacy files have data_off 0, so this is a
	// no-op for them and the same code path serves both formats.
	if (info->data_off) {
		res = f_lseek(&f, info->data_off);
		if (res != FR_OK) { f_close(&f); return 1; }
	}

	uint32_t left = info->data_size;

	while (left) {
		uint32_t n = (left > sizeof(buf)) ? (uint32_t)sizeof(buf) : left;
		res = f_read(&f, buf, n, &br);
		if (res != FR_OK || br != n) { f_close(&f); return 1; }
		memcpy(dst_ptr, buf, n);
		dst_ptr += n;
		left -= n;
	}

	f_close(&f);

	// Nothing else zeroes .bss on this OS -- there is no crt0 doing it,
	// which is why the old format shipped it as literal zeros in the
	// file. Doing it here is what makes dropping them safe.
	if (info->bss_size) memset(dst_ptr, 0, info->bss_size);

	return 0;

}
