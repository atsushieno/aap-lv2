
#ifndef __ABSTRACT_H_INCLUDED__
#define __ABSTRACT_H_INCLUDED__

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

#define AAP_PUBLIC_API __attribute__((visibility("default")))

/* nothing for desktop, AAssetManager* for Android */
void AAP_PUBLIC_API abstract_set_io_context (void* ioContext);

/* serd specific */
void* abstract_fopen(const char* path, const char* mode);
int abstract_fread(void *ptr, size_t size, size_t count, void* stream);
int abstract_fwrite(const void *ptr, size_t size, size_t count, void* stream);
int abstract_error_vfprintf (const char *format, va_list arg);
int abstract_error_fprintf (const char *format, ...);
int abstract_ferror (void* stream);
int abstract_fclose (void* stream);
int abstract_getc (void* stream);

/* lilv-specific */
int abstract_ftell(void *stream);
int abstract_fseek(void* stream, long offset, int origin);

void abstract_dir_for_each(const char* path,
                           void*       data,
                           void (*f)(const char* path, const char* name, void* data));

#define FILE void
#define fopen abstract_fopen
#define fread abstract_fread
#define fwrite abstract_fwrite
#define ferror abstract_ferror
#define fclose abstract_fclose
#define getc abstract_getc
#define ftell abstract_ftell
#define fseek abstract_fseek

#endif
