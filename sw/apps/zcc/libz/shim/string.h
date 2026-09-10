#ifndef _STRING_H
typedef unsigned int size_t;
#define _STRING_H
unsigned strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, unsigned n);
char *strcat(char *d, const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
void *memchr(const void *s, int c, unsigned n);
#endif
