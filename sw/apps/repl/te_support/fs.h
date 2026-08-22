#ifndef FS_H
#define FS_H
int fs_size(char *filename);
char *fs_mallocfile(char *filename);
int fs_write_file(char *filename, char *buf, int len);
int getch(void);
#endif
