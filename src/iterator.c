/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "iterator.h"
#include "tree.h"
#include "ignore.h"
#include "buffer.h"
#include "git2/submodule.h"
#include <ctype.h>

#define ITERATOR_SET_CB(P,NAME_LC) do { \
	(P)->cb.current = NAME_LC ## _iterator__current; \
	(P)->cb.advance = NAME_LC ## _iterator__advance; \
	(P)->cb.advance_into = NAME_LC ## _iterator__advance_into; \
	(P)->cb.seek    = NAME_LC ## _iterator__seek; \
	(P)->cb.reset   = NAME_LC ## _iterator__reset; \
	(P)->cb.at_end  = NAME_LC ## _iterator__at_end; \
	(P)->cb.free    = NAME_LC ## _iterator__free; \
	} while (0)

#define ITERATOR_CASE_FLAGS \
	(GIT_ITERATOR_IGNORE_CASE | GIT_ITERATOR_DONT_IGNORE_CASE)

#define ITERATOR_BASE_INIT(P,NAME_LC,NAME_UC) do { \
	(P) = git__calloc(1, sizeof(NAME_LC ## _iterator)); \
	GITERR_CHECK_ALLOC(P); \
	(P)->base.type    = GIT_ITERATOR_TYPE_ ## NAME_UC; \
	(P)->base.cb    = &(P)->cb; \
	ITERATOR_SET_CB(P,NAME_LC); \
	(P)->base.start   = start ? git__strdup(start) : NULL; \
	(P)->base.end     = end ? git__strdup(end) : NULL; \
	if ((start && !(P)->base.start) || (end && !(P)->base.end)) { \
		git__free(P); return -1; } \
	(P)->base.prefixcomp = git__prefixcmp; \
	(P)->base.flags = flags & ~ITERATOR_CASE_FLAGS; \
	if ((P)->base.flags & GIT_ITERATOR_DONT_AUTOEXPAND) \
		(P)->base.flags |= GIT_ITERATOR_INCLUDE_TREES; \
	} while (0)

#define iterator__flag(I,F) ((((git_iterator *)(I))->flags & (F)) != 0)

#define iterator__ignore_case(I)     iterator__flag(I,GIT_ITERATOR_IGNORE_CASE)
#define iterator__include_trees(I)   iterator__flag(I,GIT_ITERATOR_INCLUDE_TREES)
#define iterator__dont_autoexpand(I) iterator__flag(I,GIT_ITERATOR_DONT_AUTOEXPAND)
#define iterator__do_autoexpand(I) !iterator__flag(I,GIT_ITERATOR_DONT_AUTOEXPAND)

#define iterator__end(I)           ((git_iterator *)(I))->end
#define iterator__past_end(I,PATH) (iterator__end(I) && ((git_iterator *)(I))->prefixcomp((PATH),iterator__end(I)) > 0)


static int iterator__reset_range(
	git_iterator *iter, const char *start, const char *end)
{
	if (start) {
		if (iter->start)
			git__free(iter->start);
		iter->start = git__strdup(start);
		GITERR_CHECK_ALLOC(iter->start);
	}

	if (end) {
		if (iter->end)
			git__free(iter->end);
		iter->end = git__strdup(end);
		GITERR_CHECK_ALLOC(iter->end);
	}

	return 0;
}

static int iterator_update_ignore_case(
	git_iterator *iter,
	git_iterator_flag_t flags)
{
	int error = 0, ignore_case = -1;

	if ((flags & GIT_ITERATOR_IGNORE_CASE) != 0)
		ignore_case = true;
	else if ((flags & GIT_ITERATOR_DONT_IGNORE_CASE) != 0)
		ignore_case = false;
	else {
		git_index *index;

		if (!(error = git_repository_index__weakptr(&index, iter->repo)))
			ignore_case = (index->ignore_case != false);
	}

	if (ignore_case > 0)
		iter->flags = (iter->flags | GIT_ITERATOR_IGNORE_CASE);
	else if (ignore_case == 0)
		iter->flags = (iter->flags & ~GIT_ITERATOR_IGNORE_CASE);

	iter->prefixcomp = ((iter->flags & GIT_ITERATOR_IGNORE_CASE) != 0) ?
		git__prefixcmp_icase : git__prefixcmp;

	return error;
}


static int empty_iterator__noop(const git_index_entry **e, git_iterator *i)
{
	GIT_UNUSED(i);
	if (e) *e = NULL;
	return 0;
}

static int empty_iterator__seek(git_iterator *iter, const char *prefix)
{
	GIT_UNUSED(iter); GIT_UNUSED(prefix);
	return -1;
}

static int empty_iterator__reset(
	git_iterator *iter, const char *start, const char *end)
{
	GIT_UNUSED(iter); GIT_UNUSED(start); GIT_UNUSED(end);
	return 0;
}

static int empty_iterator__at_end(git_iterator *iter)
{
	GIT_UNUSED(iter);
	return 1;
}

static void empty_iterator__free(git_iterator *iter)
{
	GIT_UNUSED(iter);
}

typedef struct {
	git_iterator base;
	git_iterator_callbacks cb;
} empty_iterator;

int git_iterator_for_nothing(
	git_iterator **iter,
	git_iterator_flag_t flags,
	const char *start,
	const char *end)
{
	empty_iterator *i = git__calloc(1, sizeof(empty_iterator));
	GITERR_CHECK_ALLOC(i);

#define empty_iterator__current empty_iterator__noop
#define empty_iterator__advance empty_iterator__noop
#define empty_iterator__advance_into empty_iterator__noop

	ITERATOR_BASE_INIT(i, empty, EMPTY);

	if ((flags & GIT_ITERATOR_IGNORE_CASE) != 0)
		i->base.flags |= GIT_ITERATOR_IGNORE_CASE;

	*iter = (git_iterator *)i;

	return 0;
}


typedef struct tree_iterator_frame tree_iterator_frame;
struct tree_iterator_frame {
	tree_iterator_frame *next, *prev;
	git_tree *tree;
	const char *start;
	size_t startlen;
	size_t index;
	/* secondary tree index for case-insensitive sort */
	void **icase_map;
	void *icase_data[GIT_FLEX_ARRAY];
};

typedef struct {
	git_iterator base;
	git_iterator_callbacks cb;
	tree_iterator_frame *stack, *tail;
	git_index_entry entry;
	git_buf path;
	bool path_has_filename;
} tree_iterator;

GIT_INLINE(const git_tree_entry *)tree_iterator__tree_entry(tree_iterator *ti)
{
	tree_iterator_frame *tf = ti->stack;

	if (tf->index >= git_tree_entrycount(tf->tree))
		return NULL;

	return git_tree_entry_byindex(
		tf->tree, tf->icase_map ? (size_t)tf->icase_map[tf->index] : tf->index);
}

static char *tree_iterator__current_filename(
	tree_iterator *ti, const git_tree_entry *te)
{
	if (!ti->path_has_filename) {
		if (git_buf_joinpath(&ti->path, ti->path.ptr, te->filename) < 0)
			return NULL;

		if (git_tree_entry__is_tree(te) && git_buf_putc(&ti->path, '/') < 0)
			return NULL;

		ti->path_has_filename = true;
	}

	return ti->path.ptr;
}

static void tree_iterator__free_frame(tree_iterator_frame *tf)
{
	if (!tf)
		return;

	git_tree_free(tf->tree);
	tf->tree = NULL;

	git__free(tf);
}

static bool tree_iterator__pop_frame(tree_iterator *ti)
{
	tree_iterator_frame *tf = ti->stack;

	/* don't free the initial tree/frame */
	if (!tf->next)
		return false;

	ti->stack = tf->next;
	ti->stack->prev = NULL;

	tree_iterator__free_frame(tf);

	return true;
}

static int tree_iterator__to_end(tree_iterator *ti)
{
	while (tree_iterator__pop_frame(ti)) /* pop all */;
	ti->stack->index = git_tree_entrycount(ti->stack->tree);
	return 0;
}

static int tree_iterator__current(
	const git_index_entry **entry, git_iterator *self)
{
	tree_iterator *ti = (tree_iterator *)self;
	const git_tree_entry *te = tree_iterator__tree_entry(ti);

	if (entry)
		*entry = NULL;

	if (te == NULL)
		return 0;

	ti->entry.mode = te->attr;
	git_oid_cpy(&ti->entry.oid, &te->oid);

	ti->entry.path = tree_iterator__current_filename(ti, te);
	if (ti->entry.path == NULL)
		return -1;

	if (iterator__past_end(ti, ti->entry.path))
		return tree_iterator__to_end(ti);

	if (entry)
		*entry = &ti->entry;

	return 0;
}

static int tree_iterator__at_end(git_iterator *self)
{
	return (tree_iterator__tree_entry((tree_iterator *)self) == NULL);
}

static int tree_iterator__icase_map_cmp(const void *a, const void *b, void *data)
{
	git_tree *tree = data;
	const git_tree_entry *te1 = git_tree_entry_byindex(tree, (size_t)a);
	const git_tree_entry *te2 = git_tree_entry_byindex(tree, (size_t)b);

	return te1 ? (te2 ? git_tree_entry_icmp(te1, te2) : 1) : -1;
}

static int tree_iterator__frame_start_icmp(const void *key, const void *el)
{
	const tree_iterator_frame *tf = (const tree_iterator_frame *)key;
	const git_tree_entry *te = git_tree_entry_byindex(tf->tree, (size_t)el);
	size_t minlen = min(tf->startlen, te->filename_len);

	return git__strncasecmp(tf->start, te->filename, minlen);
}

static void tree_iterator__frame_seek_start(tree_iterator_frame *tf)
{
	if (!tf->start)
		tf->index = 0;
	else if (!tf->icase_map)
		tf->index = git_tree__prefix_position(tf->tree, tf->start);
	else {
		if (!git__bsearch(
				tf->icase_map, git_tree_entrycount(tf->tree),
				tf, tree_iterator__frame_start_icmp, &tf->index))
		{
			while (tf->index > 0) {
				/* move back while previous entry is still prefixed */
				if (tree_iterator__frame_start_icmp(
						tf, (const void *)(tf->index - 1)))
					break;
				tf->index--;
			}
		}
	}
}

static int tree_iterator__push_frame(
	tree_iterator *ti, git_tree *tree, const char *start)
{
	size_t i, max_i = git_tree_entrycount(tree);
	tree_iterator_frame *tf =
		git__calloc(1, sizeof(tree_iterator_frame) + max_i * sizeof(void *));
	GITERR_CHECK_ALLOC(tf);

	tf->tree  = tree;

	tf->next  = ti->stack;
	ti->stack = tf;
	if (tf->next)
		tf->next->prev = tf;

	if (start && *start) {
		tf->start    = start;
		tf->startlen = strlen(start);
	}

	ti->path_has_filename = false;

	if (!max_i)
		return 0;

	/* build secondary index if iterator is case-insensitive */
	if (iterator__ignore_case(ti)) {
		tf->icase_map = tf->icase_data;

		for (i = 0; i < max_i; ++i)
			tf->icase_map[i] = (void *)i;

		git__tsort_r(
			tf->icase_map, max_i, tree_iterator__icase_map_cmp, tf->tree);
	}

	tree_iterator__frame_seek_start(tf);

	return 0;
}

static int tree_iterator__expand_tree(tree_iterator *ti)
{
	int error;
	git_tree *subtree;
	const git_tree_entry *te = tree_iterator__tree_entry(ti);
	const char *relpath;

	while (te != NULL && git_tree_entry__is_tree(te)) {
		relpath = tree_iterator__current_filename(ti, te);

		/* check that we have not passed the range end */
		if (iterator__past_end(ti, relpath))
			return tree_iterator__to_end(ti);

		if ((error = git_tree_lookup(&subtree, ti->base.repo, &te->oid)) < 0)
			return error;

		relpath = NULL;

		/* apply range start to new frame if relevant */
		if (ti->stack->start &&
			ti->base.prefixcomp(ti->stack->start, te->filename) == 0)
		{
			if (ti->stack->start[te->filename_len] == '/')
				relpath = ti->stack->start + te->filename_len + 1;
		}

		if ((error = tree_iterator__push_frame(ti, subtree, relpath)) < 0)
			return error;

		/* if including trees, then one expansion is always enough */
		if (iterator__include_trees(ti))
			break;

		te = tree_iterator__tree_entry(ti);
	}

	return 0;
}

static int tree_iterator__advance_into(
	const git_index_entry **entry, git_iterator *self)
{
	int error = 0;
	tree_iterator *ti = (tree_iterator *)self;
	const git_tree_entry *te = tree_iterator__tree_entry(ti);

	if (entry)
		*entry = NULL;

	/* if DONT_AUTOEXPAND is off, the following will always be false */
	if (te && git_tree_entry__is_tree(te))
		error = tree_iterator__expand_tree(ti);

	if (!error && entry)
		error = tree_iterator__current(entry, self);

	return error;
}

static int tree_iterator__advance(
	const git_index_entry **entry, git_iterator *self)
{
	tree_iterator *ti = (tree_iterator *)self;
	const git_tree_entry *te = tree_iterator__tree_entry(ti);

	if (entry != NULL)
		*entry = NULL;

	/* given include_trees & autoexpand, we might have to go into a tree */
	if (te && git_tree_entry__is_tree(te) && iterator__do_autoexpand(ti))
		return tree_iterator__advance_into(entry, self);

	if (ti->path_has_filename) {
		git_buf_rtruncate_at_char(&ti->path, '/');
		ti->path_has_filename = false;
	}

	while (1) {
		++ti->stack->index;

		if ((te = tree_iterator__tree_entry(ti)) != NULL)
			break;

		if (!tree_iterator__pop_frame(ti))
			break; /* no frames left to pop */

		git_buf_rtruncate_at_char(&ti->path, '/');
	}

	if (te && git_tree_entry__is_tree(te) && !iterator__include_trees(ti))
		return tree_iterator__advance_into(entry, self);

	return tree_iterator__current(entry, self);
}

static int tree_iterator__seek(git_iterator *self, const char *prefix)
{
	GIT_UNUSED(self);
	GIT_UNUSED(prefix);
	/* pop stack until matches prefix */
	/* seek item in current frame matching prefix */
	/* push stack which matches prefix */
	return -1;
}

static void tree_iterator__free(git_iterator *self)
{
	tree_iterator *ti = (tree_iterator *)self;

	while (tree_iterator__pop_frame(ti)) /* pop all */;

	tree_iterator__free_frame(ti->stack);
	ti->stack = ti->tail = NULL;

	git_buf_free(&ti->path);
}

static int tree_iterator__reset(
	git_iterator *self, const char *start, const char *end)
{
	tree_iterator *ti = (tree_iterator *)self;

	while (tree_iterator__pop_frame(ti)) /* pop all */;

	if (iterator__reset_range(self, start, end) < 0)
		return -1;

	/* reset start position */
	tree_iterator__frame_seek_start(ti->stack);

	git_buf_clear(&ti->path);
	ti->path_has_filename = false;

	if (iterator__do_autoexpand(ti))
		return tree_iterator__expand_tree(ti);

	return 0;
}

int git_iterator_for_tree(
	git_iterator **iter,
	git_tree *tree,
	git_iterator_flag_t flags,
	const char *start,
	const char *end)
{
	int error;
	tree_iterator *ti;

	if (tree == NULL)
		return git_iterator_for_nothing(iter, flags, start, end);

	if ((error = git_tree__dup(&tree, tree)) < 0)
		return error;

	ITERATOR_BASE_INIT(ti, tree, TREE);

	ti->base.repo = git_tree_owner(tree);

	if ((error = iterator_update_ignore_case((git_iterator *)ti, flags)) < 0 ||
		(error = tree_iterator__push_frame(ti, tree, ti->base.start)) < 0)
		goto fail;

	if (iterator__do_autoexpand(ti))
		error = tree_iterator__expand_tree(ti);

	*iter = (git_iterator *)ti;
	return 0;

fail:
	git_iterator_free((git_iterator *)ti);
	return error;
}


typedef struct {
	git_iterator base;
	git_iterator_callbacks cb;
	git_index *index;
	size_t current;
	/* when not in autoexpand mode, use these to represent tree state */
	git_buf partial;
	size_t partial_pos;
	char restore_terminator;
	git_index_entry tree_entry;
} index_iterator;

static const git_index_entry *index_iterator__index_entry(index_iterator *ii)
{
	const git_index_entry *ie = git_index_get_byindex(ii->index, ii->current);

	if (ie != NULL && iterator__past_end(ii, ie->path)) {
		ii->current = git_index_entrycount(ii->index);
		ie = NULL;
	}

	return ie;
}

static void index_iterator__skip_conflicts(index_iterator *ii)
{
	const git_index_entry *ie;

	while ((ie = index_iterator__index_entry(ii)) != NULL &&
		   git_index_entry_stage(ie) != 0)
		ii->current++;
}

static void index_iterator__next_prefix_tree(index_iterator *ii)
{
	const char *slash;

	if (!iterator__include_trees(ii))
		return;

	slash = strchr(&ii->partial.ptr[ii->partial_pos], '/');

	if (slash != NULL) {
		ii->partial_pos = (slash - ii->partial.ptr) + 1;
		ii->restore_terminator = ii->partial.ptr[ii->partial_pos];
		ii->partial.ptr[ii->partial_pos] = '\0';
	} else {
		ii->partial_pos = ii->partial.size;
	}

	if (index_iterator__index_entry(ii) == NULL)
		ii->partial_pos = ii->partial.size;
}

static int index_iterator__first_prefix_tree(index_iterator *ii)
{
	const git_index_entry *ie = index_iterator__index_entry(ii);
	const char *scan, *prior, *slash;

	if (!ie || !iterator__include_trees(ii))
		return 0;

	/* find longest common prefix with prior index entry */

	for (scan = slash = ie->path, prior = ii->partial.ptr;
		 *scan && *scan == *prior; ++scan, ++prior)
		if (*scan == '/')
			slash = scan;

	if (git_buf_sets(&ii->partial, ie->path) < 0)
		return -1;

	ii->partial_pos = (slash - ie->path) + 1;

	index_iterator__next_prefix_tree(ii);

	return 0;
}

static int index_iterator__current(
	const git_index_entry **entry, git_iterator *self)
{
	index_iterator *ii = (index_iterator *)self;
	const git_index_entry *ie = git_index_get_byindex(ii->index, ii->current);

	if (ie != NULL &&
		iterator__include_trees(ii) &&
		ii->partial_pos < ii->partial.size)
	{
		ii->tree_entry.path = ii->partial.ptr;
		ie = &ii->tree_entry;
	}

	if (entry)
		*entry = ie;

	return 0;
}

static int index_iterator__at_end(git_iterator *self)
{
	index_iterator *ii = (index_iterator *)self;
	return (ii->current >= git_index_entrycount(ii->index));
}

static int index_iterator__advance(
	const git_index_entry **entry, git_iterator *self)
{
	index_iterator *ii = (index_iterator *)self;
	size_t entrycount = git_index_entrycount(ii->index);
	const git_index_entry *ie;

	if (iterator__include_trees(ii) &&
		ii->partial_pos < ii->partial.size)
	{
		if (iterator__do_autoexpand(ii)) {
			ii->partial.ptr[ii->partial_pos] = ii->restore_terminator;
			index_iterator__next_prefix_tree(ii);
		} else {
			/* advance until we find entry that does not share this prefix */
			while (ii->current < entrycount) {
				ii->current++;

				if (!(ie = git_index_get_byindex(ii->index, ii->current)) ||
					ii->base.prefixcomp(ie->path, ii->partial.ptr) != 0)
					break;
			}

			if (index_iterator__first_prefix_tree(ii) < 0)
				return -1;
		}
	} else {
		if (ii->current < entrycount)
			ii->current++;

		if (index_iterator__first_prefix_tree(ii) < 0)
			return -1;
	}

	return index_iterator__current(entry, self);
}

static int index_iterator__advance_into(
	const git_index_entry **entry, git_iterator *self)
{
	index_iterator *ii = (index_iterator *)self;
	const git_index_entry *ie = git_index_get_byindex(ii->index, ii->current);

	if (ie != NULL &&
		iterator__include_trees(ii) &&
		ii->partial_pos < ii->partial.size)
	{
		if (ii->restore_terminator)
			ii->partial.ptr[ii->partial_pos] = ii->restore_terminator;
		index_iterator__next_prefix_tree(ii);
	}

	return index_iterator__current(entry, self);
}

static int index_iterator__seek(git_iterator *self, const char *prefix)
{
	GIT_UNUSED(self);
	GIT_UNUSED(prefix);
	/* find last item before prefix */
	return -1;
}

static int index_iterator__reset(
	git_iterator *self, const char *start, const char *end)
{
	index_iterator *ii = (index_iterator *)self;
	const git_index_entry *ie;

	if (iterator__reset_range(self, start, end) < 0)
		return -1;

	ii->current = ii->base.start ?
		git_index__prefix_position(ii->index, ii->base.start) : 0;

	index_iterator__skip_conflicts(ii);

	if ((ie = git_index_get_byindex(ii->index, ii->current)) == NULL)
		return 0;

	if (git_buf_sets(&ii->partial, ie->path) < 0)
		return -1;

	ii->partial_pos = 0;

	if (ii->base.start) {
		size_t startlen = strlen(ii->base.start);

		ii->partial_pos = (startlen > ii->partial.size) ?
			ii->partial.size : startlen;
	}

	index_iterator__next_prefix_tree(ii);

	return 0;
}

static void index_iterator__free(git_iterator *self)
{
	index_iterator *ii = (index_iterator *)self;

	git_index_free(ii->index);
	ii->index = NULL;

	git_buf_free(&ii->partial);
}

int git_iterator_for_index(
	git_iterator **iter,
	git_index  *index,
	git_iterator_flag_t flags,
	const char *start,
	const char *end)
{
	index_iterator *ii;

	ITERATOR_BASE_INIT(ii, index, INDEX);

	ii->base.repo = git_index_owner(index);

	if (index->ignore_case) {
		ii->base.flags |= GIT_ITERATOR_IGNORE_CASE;
		ii->base.prefixcomp = git__prefixcmp_icase;
	}

	ii->index = index;
	GIT_REFCOUNT_INC(index);

	git_buf_init(&ii->partial, 0);
	ii->tree_entry.mode = GIT_FILEMODE_TREE;

	index_iterator__reset((git_iterator *)ii, NULL, NULL);

	*iter = (git_iterator *)ii;

	return 0;
}


#define WORKDIR_MAX_DEPTH 100

typedef struct workdir_iterator_frame workdir_iterator_frame;
struct workdir_iterator_frame {
	workdir_iterator_frame *next;
	git_vector entries;
	size_t index;
};

typedef struct {
	git_iterator base;
	git_iterator_callbacks cb;
	workdir_iterator_frame *stack;
	int (*entrycmp)(const void *pfx, const void *item);
	git_ignores ignores;
	git_index_entry entry;
	git_buf path;
	size_t root_len;
	int is_ignored;
	int depth;
} workdir_iterator;

GIT_INLINE(bool) path_is_dotgit(const git_path_with_stat *ps)
{
	if (!ps)
		return false;
	else {
		const char *path = ps->path;
		size_t len  = ps->path_len;

		if (len < 4)
			return false;
		if (path[len - 1] == '/')
			len--;
		if (tolower(path[len - 1]) != 't' ||
			tolower(path[len - 2]) != 'i' ||
			tolower(path[len - 3]) != 'g' ||
			tolower(path[len - 4]) != '.')
			return false;
		return (len == 4 || path[len - 5] == '/');
	}
}

static workdir_iterator_frame *workdir_iterator__alloc_frame(
	workdir_iterator *wi)
{
	workdir_iterator_frame *wf = git__calloc(1, sizeof(workdir_iterator_frame));
	git_vector_cmp entry_compare = CASESELECT(
		(wi->base.flags & GIT_ITERATOR_IGNORE_CASE) != 0,
		git_path_with_stat_cmp_icase, git_path_with_stat_cmp);

	if (wf == NULL)
		return NULL;

	if (git_vector_init(&wf->entries, 0, entry_compare) != 0) {
		git__free(wf);
		return NULL;
	}

	return wf;
}

static void workdir_iterator__free_frame(workdir_iterator_frame *wf)
{
	unsigned int i;
	git_path_with_stat *path;

	git_vector_foreach(&wf->entries, i, path)
		git__free(path);
	git_vector_free(&wf->entries);
	git__free(wf);
}

static int workdir_iterator__update_entry(workdir_iterator *wi);

static int workdir_iterator__entry_cmp_case(const void *pfx, const void *item)
{
	const git_path_with_stat *ps = item;
	return git__prefixcmp((const char *)pfx, ps->path);
}

static int workdir_iterator__entry_cmp_icase(const void *pfx, const void *item)
{
	const git_path_with_stat *ps = item;
	return git__prefixcmp_icase((const char *)pfx, ps->path);
}

static void workdir_iterator__seek_frame_start(
	workdir_iterator *wi, workdir_iterator_frame *wf)
{
	if (!wf)
		return;

	if (wi->base.start)
		git_vector_bsearch2(
			&wf->index, &wf->entries, wi->entrycmp, wi->base.start);
	else
		wf->index = 0;

	if (path_is_dotgit(git_vector_get(&wf->entries, wf->index)))
		wf->index++;
}

static int workdir_iterator__expand_dir(workdir_iterator *wi)
{
	int error;
	workdir_iterator_frame *wf;

	if (++(wi->depth) > WORKDIR_MAX_DEPTH) {
		giterr_set(GITERR_REPOSITORY, "Working directory is too deep");
		return -1;
	}

	wf = workdir_iterator__alloc_frame(wi);
	GITERR_CHECK_ALLOC(wf);

	error = git_path_dirload_with_stat(
		wi->path.ptr, wi->root_len, iterator__ignore_case(wi),
		wi->base.start, wi->base.end, &wf->entries);

	if (error < 0 || wf->entries.length == 0) {
		workdir_iterator__free_frame(wf);
		return GIT_ENOTFOUND;
	}

	workdir_iterator__seek_frame_start(wi, wf);

	/* only push new ignores if this is not top level directory */
	if (wi->stack != NULL) {
		ssize_t slash_pos = git_buf_rfind_next(&wi->path, '/');
		(void)git_ignore__push_dir(&wi->ignores, &wi->path.ptr[slash_pos + 1]);
	}

	wf->next  = wi->stack;
	wi->stack = wf;

	return workdir_iterator__update_entry(wi);
}

static int workdir_iterator__current(
	const git_index_entry **entry, git_iterator *self)
{
	workdir_iterator *wi = (workdir_iterator *)self;
	*entry = (wi->entry.path == NULL) ? NULL : &wi->entry;
	return 0;
}

static int workdir_iterator__at_end(git_iterator *self)
{
	return (((workdir_iterator *)self)->entry.path == NULL);
}

static int workdir_iterator__advance_into(
	const git_index_entry **entry, git_iterator *iter)
{
	int error = 0;
	workdir_iterator *wi = (workdir_iterator *)iter;

	if (entry)
		*entry = NULL;

	/* workdir iterator will allow you to explicitly advance into a
	 * commit/submodule (as well as a tree) to avoid some cases where an
	 * entry is mislabeled as a submodule in the working directory
	 */
	if (wi->entry.path != NULL &&
		(wi->entry.mode == GIT_FILEMODE_TREE ||
		 wi->entry.mode == GIT_FILEMODE_COMMIT))
		/* returns GIT_ENOTFOUND if the directory is empty */
		error = workdir_iterator__expand_dir(wi);

	if (!error && entry)
		error = workdir_iterator__current(entry, iter);

	return error;
}

static int workdir_iterator__advance(
	const git_index_entry **entry, git_iterator *self)
{
	int error = 0;
	workdir_iterator *wi = (workdir_iterator *)self;
	workdir_iterator_frame *wf;
	git_path_with_stat *next;

	/* given include_trees & autoexpand, we might have to go into a tree */
	if (iterator__do_autoexpand(wi) &&
		wi->entry.path != NULL &&
		wi->entry.mode == GIT_FILEMODE_TREE)
	{
		error = workdir_iterator__advance_into(entry, self);

		/* continue silently past empty directories if autoexpanding */
		if (error != GIT_ENOTFOUND)
			return error;
		giterr_clear();
		error = 0;
	}

	if (entry != NULL)
		*entry = NULL;

	while (wi->entry.path != NULL) {
		wf   = wi->stack;
		next = git_vector_get(&wf->entries, ++wf->index);

		if (next != NULL) {
			/* match git's behavior of ignoring anything named ".git" */
			if (path_is_dotgit(next))
				continue;
			/* else found a good entry */
			break;
		}

		/* pop stack if anything is left to pop */
		if (!wf->next) {
			memset(&wi->entry, 0, sizeof(wi->entry));
			return 0;
		}

		wi->stack = wf->next;
		workdir_iterator__free_frame(wf);
		git_ignore__pop_dir(&wi->ignores);
	}

	error = workdir_iterator__update_entry(wi);

	if (!error && entry != NULL)
		error = workdir_iterator__current(entry, self);

	return error;
}

static int workdir_iterator__seek(git_iterator *self, const char *prefix)
{
	GIT_UNUSED(self);
	GIT_UNUSED(prefix);
	/* pop stack until matching prefix */
	/* find prefix item in current frame */
	/* push subdirectories as deep as possible while matching */
	return 0;
}

static int workdir_iterator__reset(
	git_iterator *self, const char *start, const char *end)
{
	workdir_iterator *wi = (workdir_iterator *)self;

	while (wi->stack != NULL && wi->stack->next != NULL) {
		workdir_iterator_frame *wf = wi->stack;
		wi->stack = wf->next;
		workdir_iterator__free_frame(wf);
		git_ignore__pop_dir(&wi->ignores);
	}

	if (iterator__reset_range(self, start, end) < 0)
		return -1;

	workdir_iterator__seek_frame_start(wi, wi->stack);

	return workdir_iterator__update_entry(wi);
}

static void workdir_iterator__free(git_iterator *self)
{
	workdir_iterator *wi = (workdir_iterator *)self;

	while (wi->stack != NULL) {
		workdir_iterator_frame *wf = wi->stack;
		wi->stack = wf->next;
		workdir_iterator__free_frame(wf);
	}

	git_ignore__free(&wi->ignores);
	git_buf_free(&wi->path);
}

static int workdir_iterator__update_entry(workdir_iterator *wi)
{
	int error;
	git_path_with_stat *ps =
		git_vector_get(&wi->stack->entries, wi->stack->index);

	git_buf_truncate(&wi->path, wi->root_len);
	memset(&wi->entry, 0, sizeof(wi->entry));

	if (!ps)
		return 0;

	/* skip over .git entries */
	if (path_is_dotgit(ps))
		return workdir_iterator__advance(NULL, (git_iterator *)wi);

	if (git_buf_put(&wi->path, ps->path, ps->path_len) < 0)
		return -1;

	if (iterator__past_end(wi, wi->path.ptr + wi->root_len))
		return 0;

	wi->entry.path = ps->path;

	wi->is_ignored = -1;

	git_index_entry__init_from_stat(&wi->entry, &ps->st);

	/* need different mode here to keep directories during iteration */
	wi->entry.mode = git_futils_canonical_mode(ps->st.st_mode);

	/* if this is a file type we don't handle, treat as ignored */
	if (wi->entry.mode == 0) {
		wi->is_ignored = 1;
		return 0;
	}

	/* if this isn't a tree, then we're done */
	if (wi->entry.mode != GIT_FILEMODE_TREE)
		return 0;

	/*
	 * detect submodules && implement auto-expand, etc.
	 */

	error = git_submodule_lookup(NULL, wi->base.repo, wi->entry.path);
	if (error == GIT_ENOTFOUND)
		giterr_clear();

	/* if submodule lookup succeeded, mark GITLINK and remove trailing slash */
	if (error == 0) {
		size_t len = strlen(wi->entry.path);
		assert(wi->entry.path[len - 1] == '/');
		wi->entry.path[len - 1] = '\0';
		wi->entry.mode = S_IFGITLINK;
		return 0;
	}

	if (iterator__include_trees(wi))
		return 0;

	return workdir_iterator__advance_into(NULL, (git_iterator *)wi);
}

int git_iterator_for_workdir(
	git_iterator **iter,
	git_repository *repo,
	git_iterator_flag_t flags,
	const char *start,
	const char *end)
{
	int error;
	workdir_iterator *wi;

	assert(iter && repo);

	if ((error = git_repository__ensure_not_bare(
			repo, "scan working directory")) < 0)
		return error;

	ITERATOR_BASE_INIT(wi, workdir, WORKDIR);
	wi->base.repo = repo;

	if ((error = iterator_update_ignore_case((git_iterator *)wi, flags)) < 0)
		goto fail;

	if (git_buf_sets(&wi->path, git_repository_workdir(repo)) < 0 ||
		git_path_to_dir(&wi->path) < 0 ||
		git_ignore__for_path(repo, "", &wi->ignores) < 0)
	{
		git__free(wi);
		return -1;
	}

	wi->root_len = wi->path.size;
	wi->entrycmp = iterator__ignore_case(wi) ?
		workdir_iterator__entry_cmp_icase : workdir_iterator__entry_cmp_case;

	if ((error = workdir_iterator__expand_dir(wi)) < 0) {
		if (error != GIT_ENOTFOUND)
			goto fail;
		giterr_clear();
	}

	*iter = (git_iterator *)wi;
	return 0;

fail:
	git_iterator_free((git_iterator *)wi);
	return error;
}


void git_iterator_free(git_iterator *iter)
{
	if (iter == NULL)
		return;

	iter->cb->free(iter);

	git__free(iter->start);
	git__free(iter->end);

	memset(iter, 0, sizeof(*iter));

	git__free(iter);
}

int git_iterator_set_ignore_case(git_iterator *iter, bool ignore_case)
{
	bool current_ignore_case = ((iter->flags & GIT_ITERATOR_IGNORE_CASE) != 0);
	bool desire_ignore_case  = (ignore_case != 0);

	if (current_ignore_case == desire_ignore_case)
		return 0;

	if (iter->type == GIT_ITERATOR_TYPE_EMPTY) {
		if (desire_ignore_case)
			iter->flags |= GIT_ITERATOR_IGNORE_CASE;
		else
			iter->flags &= ~GIT_ITERATOR_IGNORE_CASE;
	} else {
		giterr_set(GITERR_INVALID,
			"Cannot currently set ignore case on non-empty iterators");
		return -1;
	}

	return 0;
}

git_index *git_iterator_get_index(git_iterator *iter)
{
	if (iter->type == GIT_ITERATOR_TYPE_INDEX)
		return ((index_iterator *)iter)->index;
	return NULL;
}

int git_iterator_current_tree_entry(
	const git_tree_entry **tree_entry, git_iterator *iter)
{
	*tree_entry = (iter->type != GIT_ITERATOR_TYPE_TREE) ? NULL :
		tree_iterator__tree_entry((tree_iterator *)iter);
	return 0;
}

int git_iterator_current_parent_tree(
	const git_tree **tree_ptr,
	git_iterator *iter,
	const char *parent_path)
{
	tree_iterator *ti = (tree_iterator *)iter;
	tree_iterator_frame *tf;
	const char *scan = parent_path;
	int (*strncomp)(const char *a, const char *b, size_t sz);

	if (iter->type != GIT_ITERATOR_TYPE_TREE || ti->stack == NULL)
		goto notfound;

	strncomp = ((iter->flags & GIT_ITERATOR_IGNORE_CASE) != 0) ?
		git__strncasecmp : git__strncmp;

	for (tf = ti->tail; tf != NULL; tf = tf->prev) {
		const git_tree_entry *te;

		if (!*scan) {
			*tree_ptr = tf->tree;
			return 0;
		}

		te = git_tree_entry_byindex(tf->tree,
			tf->icase_map ? (size_t)tf->icase_map[tf->index] : tf->index);

		if (strncomp(scan, te->filename, te->filename_len) != 0)
			goto notfound;

		scan += te->filename_len;

		if (*scan) {
			if (*scan != '/')
				goto notfound;
			scan++;
		}
	}

notfound:
	*tree_ptr = NULL;
	return 0;
}

bool git_iterator_current_is_ignored(git_iterator *iter)
{
	workdir_iterator *wi = (workdir_iterator *)iter;

	if (iter->type != GIT_ITERATOR_TYPE_WORKDIR)
		return false;

	if (wi->is_ignored != -1)
		return (bool)wi->is_ignored;

	if (git_ignore__lookup(&wi->ignores, wi->entry.path, &wi->is_ignored) < 0)
		wi->is_ignored = true;

	return (bool)wi->is_ignored;
}

int git_iterator_current_oid(git_oid *oid_out, git_iterator *iter)
{
	GIT_UNUSED(iter);
	memset(oid_out, 0, sizeof(*oid_out));
	return 0;
}

int git_iterator_cmp(git_iterator *iter, const char *path_prefix)
{
	const git_index_entry *entry;

	/* a "done" iterator is after every prefix */
	if (git_iterator_current(&entry, iter) < 0 ||
		entry == NULL)
		return 1;

	/* a NULL prefix is after any valid iterator */
	if (!path_prefix)
		return -1;

	return iter->prefixcomp(entry->path, path_prefix);
}

int git_iterator_current_workdir_path(git_buf **path, git_iterator *iter)
{
	workdir_iterator *wi = (workdir_iterator *)iter;

	if (iter->type != GIT_ITERATOR_TYPE_WORKDIR || !wi->entry.path)
		*path = NULL;
	else
		*path = &wi->path;

	return 0;
}

