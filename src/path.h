/*
 * Copyright (C) 2009-2011 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_path_h__
#define INCLUDE_path_h__

#include "common.h"

/**
 * Path object
 *
 * Dynamically reallocated path object
 */
typedef struct { /* path buffer */
	char*  data;
	size_t size;
} git_path;

#define GIT_PATH_INIT			{NULL,0}
#define GIT_PATH_INIT_STR(S)	{(S)?git__strdup(S):NULL,(S)?strlen(S)+1:0}
/* the ability to prealloc a git_path is useful while transitioning code */
#define GIT_PATH_INIT_N(N)		{(N)>0?git__calloc(N,1):NULL,N}

extern void git_path_free(git_path *path);
extern int git_path_realloc(git_path *path, size_t newsize);
extern void git_path_swap(git_path *path_a, git_path *path_b);
extern char* git_path_take_data(git_path *path);

extern int git_path_strcpy(git_path *path, const char *str);
extern int git_path_strncat(git_path *path, const char *str, size_t n);

GIT_INLINE(int) git_path_strcat(git_path *path, const char *str)
{
	return git_path_strncat(path, str, UINT_MAX);
}

GIT_INLINE(int) git_path_append(git_path *tgt, const git_path *src)
{
	return git_path_strncat(tgt, src->data, src->size);
}

/*
 * The dirname() function shall take a pointer to a character string
 * that contains a pathname, and return a pointer to a string that is a
 * pathname of the parent directory of that file. Trailing '/' characters
 * in the path are not counted as part of the path.
 *
 * If path does not contain a '/', then dirname() shall return a pointer to
 * the string ".". If path is a null pointer or points to an empty string,
 * dirname() shall return a pointer to the string "." .
 *
 * The `git_path_dirname` implementation is thread safe. The returned
 * string must be manually free'd.
 *
 * The `git_path_dirname_r` implementation expects an initialized git_path
 * object which will be (re-)allocated as needed to be big enough.
 */
extern char *git_path_dirname(const char *path);
extern int git_path_dirname_r(git_path *parent_path, const char *path);

/*
 * This function returns the basename of the file, which is the last
 * part of its full name given by fname, with the drive letter and
 * leading directories stripped off. For example, the basename of
 * c:/foo/bar/file.ext is file.ext, and the basename of a:foo is foo.
 *
 * Trailing slashes and backslashes are significant: the basename of
 * c:/foo/bar/ is an empty string after the rightmost slash.
 *
 * The `git_path_basename` implementation is thread safe. The returned
 * string must be manually free'd.
 *
 * The `git_path_basename_r` implementation expects an initialized git_path
 * object which will be (re-)allocated as needed to be big enough.
 */
extern char *git_path_basename(const char *path);
extern int git_path_basename_r(git_path *base_path, const char *path);

extern const char *git_path_topdir(const char *path);

/**
 * Join path strings together into a git_path. Takes care of properly
 * fixing the middle slashes and everything.
 *
 * Note, the arguments to this function (besides the path_out) are
 * strings, not git_paths.
 *
 * The paths are joined together into path_out.
 */
extern int git_path_join_n(git_path *path_out, int npath, ...);

GIT_INLINE(int) git_path_join(git_path *path_out, const char *path_a, const char *path_b)
{
	return git_path_join_n(path_out, 2, path_a, path_b);
}

int git_path_root(const char *path);

int git_path_prettify(git_path *path_out, const char *path, const char *base);
int git_path_prettify_dir(git_path *path_out, const char *path, const char *base);

/**
 * Ensure path has a trailing slash.
 */
int git_path_as_dir(git_path *path);
void git_path_string_as_dir(char* path, size_t size);

#ifdef GIT_WIN32
GIT_INLINE(void) git_path_mkposix(char *path)
{
	while (*path) {
		if (*path == '\\')
			*path = '/';

		path++;
	}
}
#else
#	define git_path_mkposix(p) /* blank */
#endif

#endif
