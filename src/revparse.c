#include "common.h"
#include "git2/revparse.h"
#include "git2/oid.h"
#include "git2/object.h"
#include "git2/refs.h"

int git_revparse(git_object **out, git_repository *repo, const char* spec)
{
	size_t speclen = strlen(spec);
	git_reference *ref;
	git_object *obj = NULL;

	/* sha */
	git_oid oid;
	if (git_oid_fromstrn(&oid, spec, strlen(spec)) == 0) {
		if (git_object_lookup_prefix(&obj, repo, &oid, speclen, GIT_OBJ_ANY) == 0) {
			*out = obj;
			return 0;
		}
	}

	/* Fully-named ref */
	if (git_reference_lookup(&ref, repo, spec) == 0) {
		git_reference *resolved_ref;
		if (git_reference_resolve(&resolved_ref, ref) == 0) {
			if (git_object_lookup(&obj, repo, git_reference_oid(resolved_ref), GIT_OBJ_ANY) == 0) {
				*out = obj;
			}

			git_reference_free(resolved_ref);
		}

		git_reference_free(ref);

		if (obj != NULL)
			return 0;
	}

	return GIT_ENOTFOUND;
}
