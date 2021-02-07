
#if ANDROID
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <assert.h>
#endif
#include <stdio.h>
#include <dirent.h>
#include <fcntl.h>

#include "abstract_io.h"

#if ANDROID

AAssetManager *current_asset_manager;

#endif

void* file_fopen(const char* path, const char* mode)
{
	return fopen(path, mode);
}

int file_fread(void *ptr, size_t size, size_t count, void* stream)
{
	return fread(ptr, size, count, stream);
}

int file_fwrite(const void *ptr, size_t size, size_t count, void* stream)
{
	return fwrite(ptr, size, count, stream);
}

int file_error_vfprintf (const char *format, va_list arg)
{
	return vfprintf (stderr, format, arg);
}

int file_ferror (void* stream)
{
	return ferror (stream);
}

int file_fclose (void* stream)
{
	return fclose (stream);
}

int file_getc (void* stream)
{
	return getc (stream);
}

/* lilv-specific (at least it used to be, not sure by now) */

int file_ftell(void *stream)
{
	return ftell(stream);
}

int file_fseek(void* stream, long offset, int origin)
{
	return fseek(stream, offset, origin);
}


void file_dir_for_each(const char* path,
					   void*       data,
					   void (*f)(const char* path, const char* name, void* data))
{
	void *dir = opendir(path);
	if (dir) {
		for (struct dirent *entry; (entry = readdir(dir));) {
			f(path, entry->d_name, data);
		}
		closedir(dir);
	}
}


#if ANDROID

void abstract_set_io_context (void* ioContext)
{
	current_asset_manager = (AAssetManager*) ioContext;
}

/* serd-specific (at least it used to be, not sure by now) */

void* abstract_fopen(const char* path, const char* mode)
{
	if (!current_asset_manager)
		return file_fopen(path, mode);
	void *ret = AAssetManager_open(current_asset_manager, path [0] == '/' ? path + 1 : path, AASSET_MODE_RANDOM);
	return ret;
}

int abstract_fread(void *ptr, size_t size, size_t count, void* stream)
{
	if (!current_asset_manager)
		return file_fread(ptr, size, count, stream);
	return AAsset_read((AAsset*) stream, ptr, size * count) / (int) size;
}

int abstract_fwrite(const void *ptr, size_t size, size_t count, void* stream)
{
	if (!current_asset_manager)
		return file_fwrite(ptr, size, count, stream);
	puts (NULL); /* damn "undefined reference to assert()" - just cause SIGSEGV then. */
}

int abstract_error_vfprintf (const char *format, va_list arg)
{
	if (!current_asset_manager)
		return file_error_vfprintf(format, arg);
    return vfprintf (stderr, format, arg);
}

int abstract_ferror (void* stream)
{
	if (!current_asset_manager)
		return file_ferror(stream);
	/* not much we can do here */
	return ferror((FILE*) stream);
}

int abstract_fclose (void* stream)
{
	if (!current_asset_manager)
		return file_fclose(stream);
	AAsset_close((AAsset*) stream);
	return 0;
}

int abstract_getc (void* stream)
{
	if (!current_asset_manager)
		return file_getc(stream);
	char buf[1];
	if (AAsset_read(stream, &buf, 1) <= 0)
		return -1;
	return buf [0];
}

/* lilv-specific (at least it used to be, not sure by now) */

int abstract_ftell(void *stream)
{
	if (!current_asset_manager)
		return file_ftell(stream);
	AAsset *asset = (AAsset*) stream;
	return AAsset_getLength(asset) - AAsset_getRemainingLength(asset);
}

int abstract_fseek(void* stream, long offset, int origin)
{
	if (!current_asset_manager)
		return file_fseek(stream, offset, origin);
	return AAsset_seek ((AAsset*) stream, offset, origin);
}

void abstract_dir_for_each(const char* path,
                  void*       data,
                  void (*f)(const char* path, const char* name, void* data))
{
	if (!current_asset_manager)
		file_dir_for_each(path, data, f);
	else {
		/* Due to lack of feature in Android Assets API, it is impossible to
         * enumerate directories at run time (either in NDK or SDK).
         * Therefore, we do things differently - we just pass all the plugins
         * in LV2_PATH, and each entry represents a loadable plugin.
         * So here, we only validate that the asset path exists and directly load it*/
		AAssetDir *dir = AAssetManager_openDir(current_asset_manager, path);
		if (dir) {
			f(NULL, path, data);
			AAssetDir_close(dir);
		}
	}
}

#else

void abstract_set_io_context (void* ioContext)
{
}

/* serd-specific (at least it used to be, not sure by now) */

void* abstract_fopen(const char* path, const char* mode)
{
    return file_fopen(path, mode);
}

int abstract_fread(void *ptr, size_t size, size_t count, void* stream)
{
    return file_fread(ptr, size, count, stream);
}

int abstract_fwrite(const void *ptr, size_t size, size_t count, void* stream)
{
    return file_fwrite(ptr, size, count, stream);
}

int abstract_error_vfprintf (const char *format, va_list arg)
{
    return file_vfprintf (stderr, format, arg);
}

int abstract_ferror (void* stream)
{
    return file_ferror (stream);
}

int abstract_fclose (void* stream)
{
    return file_fclose (stream);
}

int abstract_getc (void* stream)
{
    return file_getc (stream);
}

/* lilv-specific (at least it used to be, not sure by now) */

int abstract_ftell(void *stream)
{
	return file_ftell(stream);
}

int abstract_fseek(void* stream, long offset, int origin)
{
	return file_fseek(stream, offset, origin);
}


void abstract_dir_for_each(const char* path,
                  void*       data,
                  void (*f)(const char* path, const char* name, void* data))
{
	file_dir_for_each(path, data, f);
}

#endif

int abstract_error_fprintf (const char *format, ...)
{
    va_list ap;
    va_start (ap, format);
    return abstract_error_vfprintf (format, ap);
}
