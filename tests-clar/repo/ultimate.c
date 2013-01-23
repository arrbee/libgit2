#include "clar_libgit2.h"
#include "posix.h"
#include "buffer.h"
#include "path.h"
#include "fileops.h"

#define PERMACACHE	"tmp/libgit2-clar-cache"

static git_repository *g_repo = NULL;

static void clone_it(const char *url)
{
	git_buf cmd = GIT_BUF_INIT, path = GIT_BUF_INIT, cache = GIT_BUF_INIT;

	git_buf_sets(&path, strrchr(url, '/') + 1);
	git_buf_truncate(&path, path.size - 4); /* remove .git */

	git_buf_joinpath(&cache, getenv("HOME"), PERMACACHE);
	if (git_path_contains(&cache, path.ptr)) {
		git_buf_joinpath(&cache, cache.ptr, path.ptr);
		cl_git_pass(git_repository_open(&g_repo, cache.ptr));
	}
	else {
		git_buf_printf(&cmd, "git clone %s", url);
		fprintf(stderr, "\nrunning: %s\n", cmd.ptr);
		cl_git_pass(system(cmd.ptr));
		cl_assert(git_path_isdir(path.ptr));
		cl_git_pass(git_repository_open(&g_repo, path.ptr));
	}

	git_buf_free(&cmd);
	git_buf_free(&path);
	git_buf_free(&cache);
}

static int ultimate_status_cb(const char *p, unsigned int s, void *payload)
{
	int *count = (int *)payload;
	(*count)++;
	GIT_UNUSED(p);
	GIT_UNUSED(s);
	return 0;
}

static void status_it(void)
{
	int count = 0;
	git_status_options opts = GIT_STATUS_OPTIONS_INIT;

	opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
		GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
		GIT_STATUS_OPT_INCLUDE_IGNORED |
		GIT_STATUS_OPT_INCLUDE_UNMODIFIED;

	cl_git_pass(
		git_status_foreach_ext(g_repo, &opts, ultimate_status_cb, &count)
	);
	cl_assert(count > 0);
}

static void revwalk_it(int revs_len, git_oid *revs)
{
	git_revwalk *walk;
	int count = 0;
	git_oid oid;
	git_commit *commit;

	cl_git_pass(git_revwalk_new(&walk, g_repo));
	cl_git_pass(git_revwalk_push_head(walk));

	while (!git_revwalk_next(&oid, walk)) {
		count++;

		cl_git_pass(git_commit_lookup(&commit, g_repo, &oid));
		cl_assert_equal_i(0, git_oid_cmp(&oid, git_commit_id(commit)));
		cl_assert(git_commit_tree_id(commit) != NULL);

		git_commit_free(commit);

		/* save off some random oids */
		if ((rand() % count) == 0)
			git_oid_cpy(&revs[rand() % revs_len], &oid);
	}

	cl_assert(count > 0);

	git_revwalk_free(walk);
}

static void checkout_it(int revs_len, git_oid *revs)
{
	git_reference *head = NULL;
	int i;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	cl_git_pass(git_repository_head(&head, g_repo));

	opts.checkout_strategy = GIT_CHECKOUT_FORCE;

	for (i = 0; i < revs_len; ++i) {
		if (git_oid_iszero(&revs[i]))
			continue;

		/*
		cl_git_pass(git_repository_set_head_detached(g_repo, &revs[i]));
		cl_git_pass(git_checkout_head(g_repo, &opts));
		*/
	}

/*
	if (revs_len > 0) {
		cl_git_pass(git_repository_set_head(g_repo, git_reference_name(head)));
		cl_git_pass(git_checkout_head(g_repo, &opts));
	}
*/
	git_reference_free(head);
}

static void clean_it(void)
{
	if (!g_repo)
		return;

	git_repository_free(g_repo);
	g_repo = NULL;
}

void test_repo_ultimate__initialize(void)
{
}

void test_repo_ultimate__cleanup(void)
{
	clean_it();
}


#ifdef _WIN32

#include <windows.h>

double cl_time(void)
{
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

#else

#include <sys/time.h>

double cl_time(void)
{
    struct timeval tv;
    struct timezone tz;
    gettimeofday(&tv, &tz);
    return (double)tv.tv_sec + tv.tv_usec * 1E-6;
}

#endif

double cl_time_elapsed(double start)
{
	return (cl_time() - start);
}

#define LOOPS 10

static void doit(const char *name)
{
	int i;
	git_oid oids[8];
	double base;
	double elapsed = 0;

	clone_it(name);

	for (i = 0; i < LOOPS; i++) {
		base = cl_time();

		status_it();

		memset(&oids, 0, sizeof(oids));
		revwalk_it(8, oids);

		checkout_it(8, oids);

		elapsed += cl_time_elapsed(base);
	}

	clean_it();

	fprintf(stderr, "\n%s -> %.2lf\n", name, elapsed / LOOPS);
}

#ifndef GIT_WIN32
void test_repo_ultimate__git(void) {
	doit("https://github.com/git/git.git");
}
/* "https://github.com/torvalds/linux.git", */
void test_repo_ultimate__libgit2(void) {
	doit("https://github.com/libgit2/libgit2.git");
}
void test_repo_ultimate__node(void) {
	doit("https://github.com/joyent/node.git");
}
void test_repo_ultimate__perl(void) {
	doit("https://github.com/mirrors/perl.git");
}
#endif
