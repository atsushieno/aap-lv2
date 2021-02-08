#include <stdio.h>

FILE* std_fopen(const char* path, const char* mode);
int std_fread(void *buf, size_t size, size_t count, FILE* file);
int std_fwrite(const void *buf, size_t size, size_t count, FILE* file);
int std_ferror(FILE* file);
int std_fclose(FILE* file);
int std_getc(FILE* file);
int std_ftell(FILE* file);
int std_fseek(FILE* file, long offset, int origin);

