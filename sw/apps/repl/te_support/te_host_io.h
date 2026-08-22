#ifndef TE_HOST_IO_H
#define TE_HOST_IO_H
void te_host_write(const char *buf, int len);
int  te_host_getch(void);
void te_host_flush(void);
#endif
