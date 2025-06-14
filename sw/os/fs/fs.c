#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs/ff.h"

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

	printf("loading %li bytes ...\n", sz);

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
