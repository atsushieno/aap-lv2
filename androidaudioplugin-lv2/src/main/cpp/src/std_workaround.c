#include <stdio.h>

FILE* std_fopen(const char* path, const char* mode) { return fopen(path,mode); }
int std_fread(void *buf, size_t size, size_t count, FILE* file) { return fread(buf, size, count, file); }
int std_fwrite(const void *buf, size_t size, size_t count, FILE* file) { return fwrite(buf, size, count, file); }
int std_ferror(FILE* file) { return ferror(file); }
int std_fclose(FILE* file) { return fclose(file); }
int std_getc(FILE* file) { return getc(file); }
int std_ftell(FILE* file) { return ftell(file); }
int std_fseek(FILE* file, long offset, int origin) { return fseek(file, offset, origin); }
