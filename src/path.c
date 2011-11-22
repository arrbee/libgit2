/*
 * Copyright (C) 2009-2011 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "common.h"
#include "path.h"
#include "posix.h"

#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

void git_path_free(git_path *path)
{
	assert(path);
	if (path->data) {
		git__free(path->data);
	}
	path->data = NULL;
	path->size = 0;
}

void git_path_swap(git_path *path_a, git_path *path_b)
{
	git_path temp = *path_a;
	*path_a = *path_b;
	*path_b = temp;
}

char* git_path_take_data(git_path *path)
{
	char *data = path->data;

	path->data = NULL;
	path->size = 0;

	return data;
}


int git_path_realloc(git_path *path, size_t newsize)
{
	assert(path);

	/* round size up to multiple of 8 (since allocator usually does that internally already */
	newsize = (newsize + 7) & (size_t)0xFFFFFFF8L;

	if (!path->data) {
		path->data = git__malloc(newsize);
		if (path->data) {
			*(path->data) = '\0';
			path->size = newsize;
		} else {
			path->size = 0;
			return git__throw(GIT_ENOMEM, "Could not expand path to %d", (int)newsize);
		}
	}
	else if (path->size < newsize) {
		path->data = git__realloc(path->data, newsize);
		if (path->data) {
			path->size = newsize;
		} else {
			path->size = 0;
			return git__throw(GIT_ENOMEM, "Could not expand path to %d", (int)newsize);
		}
	}

	return GIT_SUCCESS;
}

int git_path_strcpy(git_path *path, const char* str)
{
	int    error = GIT_SUCCESS;
	size_t len = str ? strlen(str) + 1 : 0;

	assert(path);

	if (len == 0) {
		git_path_free(path);
	} else {
		if (path->size < len)
			error = git_path_realloc(path, len);

		if (error == GIT_SUCCESS)
			memmove(path->data, str, len);
	}

	return error;
}

int git_path_strncat(git_path *path, const char* str, size_t n)
{
	assert(path);

	int	   error = GIT_SUCCESS;
	size_t old_size	 = path->data ? strlen(path->data) : 0;
	size_t add_size	 = str ? strlen(str) : 0;
	if (add_size > n) add_size = n;
	size_t null_byte = (size_t)((path->data != NULL) || (str != NULL));
	size_t new_size	 = old_size + add_size + null_byte;

	if (path->size < new_size)
		error = git_path_realloc(path, new_size);

	if (add_size > 0 && error == GIT_SUCCESS) {
		memmove(path->data + old_size, str, add_size);

		/* make sure to terminate new string */
		path->data[old_size + add_size] = '\0';
	}

	return error;
}

/*
 * Based on the Android implementation, BSD licensed.
 * Check http://android.git.kernel.org/
 */
int git_path_basename_r(git_path *base_path, const char *path)
{
	const char *endp, *startp;
	int len, result;

	/* Empty or NULL string gets treated as "." */
	if (path == NULL || *path == '\0') {
		startp = ".";
		len		= 1;
		goto Exit;
	}

	/* Strip trailing slashes */
	endp = path + strlen(path) - 1;
	while (endp > path && *endp == '/')
		endp--;

	/* All slashes becomes "/" */
	if (endp == path && *endp == '/') {
		startp = "/";
		len	= 1;
		goto Exit;
	}

	/* Find the start of the base */
	startp = endp;
	while (startp > path && *(startp - 1) != '/')
		startp--;

	len = endp - startp +1;

Exit:
	result = len;

	if (base_path == NULL) {
		return result;
	}

	if (len > (int)base_path->size - 1) {
		int error = git_path_realloc(base_path, len + 1);
		if (error != GIT_SUCCESS)
			return git__rethrow(error, "Could not get basename of '%s'", path);
	}

	if (len >= 0) {
		memmove(base_path->data, startp, len);
		base_path->data[len] = 0;
	}

	return result;
}

/*
 * Based on the Android implementation, BSD licensed.
 * Check http://android.git.kernel.org/
 */
int git_path_dirname_r(git_path* parent_path, const char *path)
{
	const char *endp;
	int result, len;

	/* Empty or NULL string gets treated as "." */
	if (path == NULL || *path == '\0') {
		path = ".";
		len = 1;
		goto Exit;
	}

	/* Strip trailing slashes */
	endp = path + strlen(path) - 1;
	while (endp > path && *endp == '/')
		endp--;

	/* Find the start of the dir */
	while (endp > path && *endp != '/')
		endp--;

	/* Either the dir is "/" or there are no slashes */
	if (endp == path) {
		path = (*endp == '/') ? "/" : ".";
		len = 1;
		goto Exit;
	}

	do {
		endp--;
	} while (endp > path && *endp == '/');

	len = endp - path +1;

#ifdef GIT_WIN32
	/* Mimic unix behavior where '/.git' returns '/': 'C:/.git' will return
		'C:/' here */

	if (len == 2 && isalpha(path[0]) && path[1] == ':') {
		len = 3;
		goto Exit;
	}
#endif

Exit:
	result = len;

	if (parent_path == NULL)
		return result;

	if (len > (int)parent_path->size - 1) {
		int error = git_path_realloc(parent_path, len + 1);
		if (error != GIT_SUCCESS)
			return git__rethrow(error, "Could not get dirname of '%s'", path);
	}

	if (len >= 0) {
		memmove(parent_path->data, path, len);
		parent_path->data[len] = 0;
	}

	return result;
}


char *git_path_dirname(const char *path)
{
	git_path dname = GIT_PATH_INIT;

	if (git_path_dirname_r(&dname, path) < GIT_SUCCESS) {
		git_path_free(&dname);
		return NULL;
	}

	return dname.data;
}

char *git_path_basename(const char *path)
{
	git_path bname = GIT_PATH_INIT;

	if (git_path_basename_r(&bname, path) < GIT_SUCCESS) {
		git_path_free(&bname);
		return NULL;
	}

	return bname.data;
}


const char *git_path_topdir(const char *path)
{
	size_t len;
	int i;

	assert(path);
	len = strlen(path);

	if (!len || path[len - 1] != '/')
		return NULL;

	for (i = len - 2; i >= 0; --i)
		if (path[i] == '/')
			break;

	return &path[i + 1];
}

int git_path_join_n(git_path *path_out, int count, ...)
{
	va_list ap;
	int i;
	char *buffer_start, *buffer_out;
	int path_size = 1; /* start with space for NULL byte */

	/* Make two passes over the args to avoid multiple path
	 * reallocations.
	 */

	va_start(ap, count);
	for (i = 0; i < count; ++i) {
		const char *path;
		int len;

		path = va_arg(ap, const char *);
		if (!*path)
			continue;

		len = strlen(path);
		path_size += len;

		if (path[len] != '/') /* make space for trailing slash */
			path_size++;
	}
	va_end(ap);

	/* perform only one alloc */
	if (git_path_realloc(path_out, path_size) < GIT_SUCCESS)
        return git__throw(GIT_ENOMEM, "Failed to expand path");

	buffer_start = buffer_out = path_out->data;

	va_start(ap, count);
	for (i = 0; i < count; ++i) {
		const char *path;
		int len;

		path = va_arg(ap, const char *);

		assert((i == 0) || path != buffer_start);

		if (i > 0 && *path == '/' && buffer_out > buffer_start && buffer_out[-1] == '/')
			path++;

		if (!*path)
			continue;

		len = strlen(path);
		memmove(buffer_out, path, len);
		buffer_out = buffer_out + len;

		if (i < count - 1 && buffer_out[-1] != '/')
			*buffer_out++ = '/';
	}
	va_end(ap);

	*buffer_out = '\0';

	return GIT_SUCCESS;
}

int git_path_root(const char *path)
{
	int offset = 0;

#ifdef GIT_WIN32
	/* Does the root of the path look like a windows drive ? */
	if (isalpha(path[0]) && (path[1] == ':'))
		offset += 2;
#endif

	if (*(path + offset) == '/')
		return offset;

	return -1;	/* Not a real error. Rather a signal than the path is not rooted */
}

int git_path_prettify(git_path *path_out, const char *path, const char *base)
{
	char *result = NULL;
	int   error = GIT_SUCCESS;

	if (base == NULL || git_path_root(path) >= 0) {
		/* allow realpath to allocate the buffer */
		result = p_realpath(path, NULL);
	} else {
		error = git_path_join(path_out, base, path);
		if (error == GIT_SUCCESS)
			result = p_realpath(path_out->data, NULL);
	}

	if (result) {
		/* transfer allocated buffer into path_out */
		git_path_free(path_out);
		path_out->data = result;
		path_out->size = strlen(result);
	} else { /* !result */
		if (error == GIT_SUCCESS)
			error = GIT_EOSERR;
	}

	return error;
}

int git_path_prettify_dir(git_path *path_out, const char *path, const char *base)
{
	int error = git_path_prettify(path_out, path, base);

	if (error == GIT_SUCCESS) {
		/* it is unfortunate, but because of how we are using p_realpath
		 * to reallocate the path_out when we prettify, the following
		 * line will always end up having to call realloc on the buffer.
		 */
		error = git_path_as_dir(path_out);
	}

	return error;
}

void git_path_string_as_dir(char* path, size_t size)
{
	size_t end = strlen(path);

	if (end && path[end - 1] != '/' && end < size) {
		path[end] = '/';
		path[end + 1] = '\0';
	}
}

int git_path_as_dir(git_path *path)
{
	int error = GIT_SUCCESS;
	size_t end = (path && path->data) ? strlen(path->data) : 0;

	if (end && path->data[end - 1] != '/') {
		error = git_path_realloc(path, end + 2);
		if (error == GIT_SUCCESS) {
			path->data[end] = '/';
			path->data[end + 1] = '\0';
		}
	}

	return error;
}
