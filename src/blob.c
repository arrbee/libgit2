/*
 * Copyright (C) 2009-2011 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "git2/common.h"
#include "git2/object.h"
#include "git2/repository.h"

#include "common.h"
#include "blob.h"

const void *git_blob_rawcontent(git_blob *blob)
{
	assert(blob);
	return blob->odb_object->raw.data;
}

size_t git_blob_rawsize(git_blob *blob)
{
	assert(blob);
	return blob->odb_object->raw.len;
}

void git_blob__free(git_blob *blob)
{
	git_odb_object_close(blob->odb_object);
	git__free(blob);
}

int git_blob__parse(git_blob *blob, git_odb_object *odb_obj)
{
	assert(blob);
	git_cached_obj_incref((git_cached_obj *)odb_obj);
	blob->odb_object = odb_obj;
	return GIT_SUCCESS;
}

int git_blob_create_frombuffer(git_oid *oid, git_repository *repo, const void *buffer, size_t len)
{
	int error;
	git_odb_stream *stream;

	if ((error = git_odb_open_wstream(&stream, repo->db, len, GIT_OBJ_BLOB)) < GIT_SUCCESS)
		return git__rethrow(error, "Failed to create blob");

	if ((error = stream->write(stream, buffer, len)) < GIT_SUCCESS) {
		stream->free(stream);
		return error;
	}

	error = stream->finalize_write(oid, stream);
	stream->free(stream);

	if (error < GIT_SUCCESS)
		return git__rethrow(error, "Failed to create blob");

	return GIT_SUCCESS;
}

int git_blob_create_fromfile(git_oid *oid, git_repository *repo, const char *path)
{
	int error = GIT_SUCCESS;
	int islnk = 0;
	int fd = 0;
	git_path full_path = GIT_PATH_INIT;
	char buffer[2048];
	git_off_t size;
	git_odb_stream *stream = NULL;
	struct stat st;

	if (repo->path_workdir == NULL)
		return git__throw(GIT_ENOTFOUND, "Failed to create blob. (No working directory found)");

	error = git_path_join(&full_path, repo->path_workdir, path);
	if (error < GIT_SUCCESS)
		goto cleanup;

	error = p_lstat(full_path.data, &st);
	if (error < 0) {
		error = git__throw(GIT_EOSERR, "Failed to stat blob. %s", strerror(errno));
		goto cleanup;
	}

	islnk = S_ISLNK(st.st_mode);
	size = st.st_size;

	if (!islnk)
		if ((fd = p_open(full_path.data, O_RDONLY)) < 0) {
			error = git__throw(GIT_ENOTFOUND, "Failed to create blob. Could not open '%s'", full_path.data);
			goto cleanup;
        }

	if ((error = git_odb_open_wstream(&stream, repo->db, (size_t)size, GIT_OBJ_BLOB)) < GIT_SUCCESS) {
        goto cleanup;
	}

	while (size > 0) {
		ssize_t read_len;

		if (!islnk)
			read_len = p_read(fd, buffer, sizeof(buffer));
		else
			read_len = p_readlink(full_path.data, buffer, sizeof(buffer));

		if (read_len < 0) {
			error = git__throw(GIT_EOSERR, "Failed to create blob. Can't read full file");
			goto cleanup;
		}

		stream->write(stream, buffer, read_len);
		size -= read_len;
	}

	error = stream->finalize_write(oid, stream);

cleanup:
    if (stream)
        stream->free(stream);
	if (!islnk && fd)
		p_close(fd);
    git_path_free(&full_path);

	return error == GIT_SUCCESS ? GIT_SUCCESS : git__rethrow(error, "Failed to create blob");
}

