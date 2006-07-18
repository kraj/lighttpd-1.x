#include <stdlib.h>
#include <string.h>

#include "mod_proxy_core_rewrites.h"
#include "log.h"

proxy_rewrite *proxy_rewrite_init(void) {
	STRUCT_INIT(proxy_rewrite, rewrite);

	rewrite->header = buffer_init();
	rewrite->match = buffer_init();
	rewrite->replace = buffer_init();

	return rewrite;

}
void proxy_rewrite_free(proxy_rewrite *rewrite) {
	if (!rewrite) return;

	if (rewrite->regex) pcre_free(rewrite->regex);

	buffer_free(rewrite->header);
	buffer_free(rewrite->match);
	buffer_free(rewrite->replace);

	free(rewrite);
}

int proxy_rewrite_set_regex(proxy_rewrite *rewrite, buffer *regex) {
	const char *errptr;
	int erroff;

	if (NULL == (rewrite->regex = pcre_compile(BUF_STR(regex),
		  0, &errptr, &erroff, NULL))) {
		
		TRACE("regex compilation for %s failed at %s", BUF_STR(regex), errptr);

		return -1;
	}

	return 0;
}


proxy_rewrites *proxy_rewrites_init(void) {
	STRUCT_INIT(proxy_rewrites, rewrites);

	return rewrites;
}

void proxy_rewrites_add(proxy_rewrites *rewrites, proxy_rewrite *rewrite) {
	ARRAY_STATIC_PREPARE_APPEND(rewrites);

	rewrites->ptr[rewrites->used++] = rewrite;
}

void proxy_rewrites_free(proxy_rewrites *rewrites) {
	if (!rewrites) return;

	free(rewrites);
}



