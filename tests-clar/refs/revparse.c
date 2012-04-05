#include "clar_libgit2.h"
#include "git2/revparse.h"


static git_repository *g_repo = NULL;

void test_refs_revparse__initialize(void)
{
	g_repo = cl_git_sandbox_init("testrepo");
}

void test_refs_revparse__cleanup(void)
{
	cl_git_sandbox_cleanup();
}


void test_refs_revparse__sha(void)
{
	git_object *obj;
	git_oid oid;
	static const char *str = "a65fedf39aefe402d3bb6e24df4d4f5fe4547750";
	cl_git_pass(git_oid_fromstr(&oid, str));

	cl_git_pass(git_revparse(&obj, g_repo, str));
	cl_assert(git_oid_cmp(&oid, git_object_id(obj)) == 0);

	cl_git_pass(git_revparse(&obj, g_repo, "a65fedf"));
	cl_assert(git_oid_cmp(&oid, git_object_id(obj)) == 0);
}


static void oid_str_cmp(const git_oid *oid, const char *str)
{
	git_oid oid2;
	cl_git_pass(git_oid_fromstr(&oid2, str));
	cl_assert(git_oid_cmp(oid, &oid2) == 0);
}

void test_refs_revparse__named_ref(void)
{
	git_object *obj;

	// HEAD
	cl_git_pass(git_revparse(&obj, g_repo, "HEAD"));
	cl_assert(git_object_type(obj) == GIT_OBJ_COMMIT);

	// refs/heads/test => e90810b8df3e80c413d903f631643c716887138d
	cl_git_pass(git_revparse(&obj, g_repo, "refs/heads/test"));
	oid_str_cmp(git_object_id(obj), "e90810b8df3e80c413d903f631643c716887138d");

	// refs/tags/test => b25fa35b38051e4ae45d4222e795f9df2e43f1d1
	cl_git_pass(git_revparse(&obj, g_repo, "refs/tags/test"));
	oid_str_cmp(git_object_id(obj), "b25fa35b38051e4ae45d4222e795f9df2e43f1d1");
}
