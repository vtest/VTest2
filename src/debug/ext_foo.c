/*
 * example vtc extension to add the 'foo' command
 */

#include "config.h"

#include <string.h>

#include "vtc.h"

static void
cmd_foo(CMD_ARGS)
{
	(void)priv;

	// fini
	if (av == NULL)
		return;

	AZ(strcmp(av[0], "foo"));
	av++;

	AN(vl);

	for (; *av != NULL; av++)
		vtc_log(vl, 1, "foo: %s", *av);
}

static struct cmds foo_cmds[1] = {{"foo", cmd_foo}};

static __attribute__((constructor)) void
foo_init(void) {
	register_top_cmds(foo_cmds, vcountof(foo_cmds));
}
