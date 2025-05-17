/*-
 * Copyright (c) 2008-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#include "vtc.h"

#include "vav.h"
#include "vnum.h"
#include "vre.h"
#include "vtim.h"

#define		MAX_TOKENS		200

volatile sig_atomic_t	vtc_error;	/* Error encountered */
int			vtc_stop;	/* Stops current test without error */
pthread_t		vtc_thread;
static struct vtclog	*vltop;

/**********************************************************************
 * Macro facility
 */

struct macro {
	unsigned		magic;
#define MACRO_MAGIC		0x803423e3
	VTAILQ_ENTRY(macro)	list;
	char			*name;
	char			*val;
};

static VTAILQ_HEAD(,macro) macro_list = VTAILQ_HEAD_INITIALIZER(macro_list);

/**********************************************************************/

static struct macro *
macro_def_int(const char *name, const char *fmt, va_list ap)
{
	struct macro *m;
	char buf[256];

	VTAILQ_FOREACH(m, &macro_list, list)
		if (!strcmp(name, m->name))
			break;
	if (m == NULL) {
		ALLOC_OBJ(m, MACRO_MAGIC);
		AN(m);
		REPLACE(m->name, name);
		AN(m->name);
		VTAILQ_INSERT_TAIL(&macro_list, m, list);
	}
	AN(m);
	vbprintf(buf, fmt, ap);
	REPLACE(m->val, buf);
	AN(m->val);
	return (m);
}


/**********************************************************************
 * This is for defining macros before we fork the child process which
 * runs the test-case.
 */

void
extmacro_def(const char *name, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)macro_def_int(name, fmt, ap);
	va_end(ap);
}

/**********************************************************************
 * Below this point is run inside the testing child-process.
 */

static pthread_mutex_t		macro_mtx;

static void
init_macro(void)
{
	struct macro *m;

	/* Dump the extmacros for completeness */
	VTAILQ_FOREACH(m, &macro_list, list)
		vtc_log(vltop, 4, "extmacro def %s=%s", m->name, m->val);

	AZ(pthread_mutex_init(&macro_mtx, NULL));
}

void
macro_def(struct vtclog *vl, const char *instance, const char *name,
    const char *fmt, ...)
{
	char buf1[256];
	struct macro *m;
	va_list ap;

	AN(fmt);

	if (instance != NULL) {
		bprintf(buf1, "%s_%s", instance, name);
		name = buf1;
	}

	AZ(pthread_mutex_lock(&macro_mtx));
	va_start(ap, fmt);
	m = macro_def_int(name, fmt, ap);
	va_end(ap);
	vtc_log(vl, 4, "macro def %s=%s", name, m->val);
	AZ(pthread_mutex_unlock(&macro_mtx));
}

void
macro_undef(struct vtclog *vl, const char *instance, const char *name)
{
	char buf1[256];
	struct macro *m;

	if (instance != NULL) {
		bprintf(buf1, "%s_%s", instance, name);
		name = buf1;
	}

	AZ(pthread_mutex_lock(&macro_mtx));
	VTAILQ_FOREACH(m, &macro_list, list)
		if (!strcmp(name, m->name))
			break;
	if (m != NULL) {
		vtc_log(vl, 4, "macro undef %s", name);
		VTAILQ_REMOVE(&macro_list, m, list);
		free(m->name);
		free(m->val);
		free(m);
	}
	AZ(pthread_mutex_unlock(&macro_mtx));
}

static char *
macro_get(const char *b, const char *e)
{
	struct macro *m;
	int l;
	char *retval = NULL;

	l = e - b;

	if (l == 4 && !memcmp(b, "date", l)) {
		double t = VTIM_real();
		retval = malloc(64);
		AN(retval);
		VTIM_format(t, retval);
		return (retval);
	}

	AZ(pthread_mutex_lock(&macro_mtx));
	VTAILQ_FOREACH(m, &macro_list, list) {
		CHECK_OBJ_NOTNULL(m, MACRO_MAGIC);
		if (!strncmp(b, m->name, l) && m->name[l] == '\0')
			break;
	}
	if (m != NULL)
		retval = strdup(m->val);
	AZ(pthread_mutex_unlock(&macro_mtx));
	return (retval);
}

struct vsb *
macro_expand(struct vtclog *vl, const char *text)
{
	struct vsb *vsb;
	const char *p, *q;
	char *m;

	vsb = VSB_new_auto();
	AN(vsb);
	while (*text != '\0') {
		p = strstr(text, "${");
		if (p == NULL) {
			VSB_cat(vsb, text);
			break;
		}
		VSB_bcat(vsb, text, p - text);
		q = strchr(p, '}');
		if (q == NULL) {
			VSB_cat(vsb, text);
			break;
		}
		assert(p[0] == '$');
		assert(p[1] == '{');
		assert(q[0] == '}');
		p += 2;
		m = macro_get(p, q);
		if (m == NULL) {
			VSB_destroy(&vsb);
			vtc_log(vl, 0, "Macro ${%.*s} not found", (int)(q - p),
			    p);
			NEEDLESS_RETURN (NULL);
		}
		VSB_printf(vsb, "%s", m);
		free(m);
		text = q + 1;
	}
	AZ(VSB_finish(vsb));
	return (vsb);
}

/**********************************************************************
 * Parse a string
 *
 * We make a copy of the string and deliberately leak it, so that all
 * the cmd functions we call don't have to strdup(3) all over the place.
 *
 * Static checkers like Coverity may bitch about this, but we don't care.
 */

void
parse_string(const char *spec, const struct cmds *cmd, void *priv,
    struct vtclog *vl)
{
	char *token_s[MAX_TOKENS], *token_e[MAX_TOKENS];
	struct vsb *token_exp[MAX_TOKENS];
	char *p, *q, *f, *buf;
	int nest_brace;
	int tn;
	const struct cmds *cp;

	AN(spec);
	buf = strdup(spec);
	AN(buf);
	for (p = buf; *p != '\0'; p++) {
		if (vtc_error || vtc_stop)
			break;
		/* Start of line */
		if (isspace(*p))
			continue;
		if (*p == '\n')
			continue;

		if (*p == '#') {
			for (; *p != '\0' && *p != '\n'; p++)
				;
			if (*p == '\0')
				break;
			continue;
		}

		q = strchr(p, '\n');
		if (q == NULL)
			q = strchr(p, '\0');
		if (q - p > 60)
			vtc_log(vl, 2, "=== %.60s...", p);
		else
			vtc_log(vl, 2, "=== %.*s", (int)(q - p), p);

		/* First content on line, collect tokens */
		tn = 0;
		f = p;
		while (*p != '\0') {
			assert(tn < MAX_TOKENS);
			if (*p == '\n') { /* End on NL */
				break;
			}
			if (isspace(*p)) { /* Inter-token whitespace */
				p++;
				continue;
			}
			if (*p == '\\' && p[1] == '\n') { /* line-cont */
				p += 2;
				continue;
			}
			if (*p == '"') { /* quotes */
				token_s[tn] = ++p;
				q = p;
				for (; *p != '\0'; p++) {
					if (*p == '"')
						break;
					if (*p == '\\') {
						p += VAV_BackSlash(p, q) - 1;
						q++;
					} else {
						if (*p == '\n')
							vtc_log(vl, 0,
				"Unterminated quoted string in line: %*.*s",
				(int)(p - f), (int)(p - f), f);
						assert(*p != '\n');
						*q++ = *p;
					}
				}
				token_e[tn++] = q;
				p++;
			} else if (*p == '{') { /* Braces */
				nest_brace = 0;
				token_s[tn] = p + 1;
				for (; *p != '\0'; p++) {
					if (*p == '{')
						nest_brace++;
					else if (*p == '}') {
						if (--nest_brace == 0)
							break;
					}
				}
				assert(*p == '}');
				token_e[tn++] = p++;
			} else { /* other tokens */
				token_s[tn] = p;
				for (; *p != '\0' && !isspace(*p); p++)
					;
				token_e[tn++] = p;
			}
		}
		assert(tn < MAX_TOKENS);
		token_s[tn] = NULL;
		for (tn = 0; token_s[tn] != NULL; tn++) {
			token_exp[tn] = NULL;
			AN(token_e[tn]);	/*lint !e771 */
			*token_e[tn] = '\0';	/*lint !e771 */
			if (NULL == strstr(token_s[tn], "${"))
				continue;
			token_exp[tn] = macro_expand(vl, token_s[tn]);
			if (vtc_error) {
				return;
			}
			token_s[tn] = VSB_data(token_exp[tn]);
			token_e[tn] = strchr(token_s[tn], '\0');
		}

		for (cp = cmd; cp->name != NULL; cp++)
			if (!strcmp(token_s[0], cp->name))
				break;
		if (cp->name == NULL) {
			vtc_log(vl, 0, "Unknown command: \"%s\"", token_s[0]);
			NEEDLESS_RETURN;
		}

		assert(cp->cmd != NULL);
		cp->cmd(token_s, priv, cmd, vl);
	}
}

/**********************************************************************
 * Reset commands (between tests)
 */

static void
reset_cmds(const struct cmds *cmd)
{

	for (; cmd->name != NULL; cmd++)
		cmd->cmd(NULL, NULL, NULL, NULL);
}

/* SECTION: varnishtest varnishtest
 *
 * This should be the first command in your vtc as it will identify the test
 * case with a short yet descriptive sentence. It takes exactly one argument, a
 * string, eg::
 *
 *         varnishtest "Check that varnishtest is actually a valid command"
 *
 * It will also print that string in the log.
 */

static void
cmd_varnishtest(CMD_ARGS)
{

	(void)priv;
	(void)cmd;
	(void)vl;

	if (av == NULL)
		return;
	AZ(strcmp(av[0], "varnishtest"));

	vtc_log(vl, 1, "TEST %s", av[1]);
	AZ(av[2]);
}

/* SECTION: shell shell
 *
 * Pass the string given as argument to a shell. If you have multiple commands
 * to run, you can use curly barces to describe a multi-lines script, eg::
 *
 *         shell {
 *                 echo begin
 *                 cat /etc/fstab
 *                 echo end
 *         }
 *
 * The vtc will fail if the return code of the shell is not 0.
 */

static void
cmd_shell(CMD_ARGS)
{
	(void)priv;
	(void)cmd;
	int r, s;

	if (av == NULL)
		return;
	AN(av[1]);
	AZ(av[2]);
	vtc_dump(vl, 4, "shell", av[1], -1);
	r = system(av[1]);
	s = WEXITSTATUS(r);
	if (s != 0)
		vtc_log(vl, 0, "CMD '%s' failed with status %d (%s)",
		    av[1], s, strerror(errno));
}

/* SECTION: err_shell err_shell
 *
 * This is very similar to the the ``shell`` command, except it takes a first
 * string as argument before the command::
 *
 *         err_shell "foo" "echo foo"
 *
 * err_shell expect the shell command to fail AND stdout to match the string,
 * failing the test case otherwise.
 */

static void
cmd_err_shell(CMD_ARGS)
{
	(void)priv;
	(void)cmd;
	struct vsb *vsb;
	FILE *fp;
	int r, c;

	if (av == NULL)
		return;
	AN(av[1]);
	AN(av[2]);
	AZ(av[3]);
	vsb = VSB_new_auto();
	AN(vsb);
	vtc_dump(vl, 4, "cmd", av[2], -1);
	fp = popen(av[2], "r");
	if (fp == NULL)
		vtc_log(vl, 0, "popen fails: %s", strerror(errno));
	do {
		c = getc(fp);
		if (c != EOF)
			VSB_putc(vsb, c);
	} while (c != EOF);
	r = pclose(fp);
	vtc_log(vl, 4, "Status = %d", WEXITSTATUS(r));
	if (WIFSIGNALED(r))
		vtc_log(vl, 4, "Signal = %d", WTERMSIG(r));
	if (WEXITSTATUS(r) == 0) {
		vtc_log(vl, 0,
		    "expected error from shell");
	}
	AZ(VSB_finish(vsb));
	vtc_dump(vl, 4, "stdout", VSB_data(vsb), VSB_len(vsb));
	if (strstr(VSB_data(vsb), av[1]) == NULL)
		vtc_log(vl, 0,
		    "Did not find expected string: (\"%s\")", av[1]);
	else
		vtc_log(vl, 4,
		    "Found expected string: (\"%s\")", av[1]);
	VSB_destroy(&vsb);
}

/**********************************************************************
 * Dump command arguments
 */

void
cmd_delay(CMD_ARGS)
{
	double f;

	(void)priv;
	(void)cmd;
	if (av == NULL)
		return;
	AN(av[1]);
	AZ(av[2]);
	f = VNUM(av[1]);
	vtc_log(vl, 3, "delaying %g second(s)", f);
	VTIM_sleep(f);
}

/**********************************************************************
 * Check random generator
 */

#define NRNDEXPECT	12
static const unsigned long random_expect[NRNDEXPECT] = {
	1804289383,	846930886,	1681692777,	1714636915,
	1957747793,	424238335,	719885386,	1649760492,
	 596516649,	1189641421,	1025202362,	1350490027
};

#define RND_NEXT_1K	0x3bdcbe30

static void
cmd_random(CMD_ARGS)
{
	uint32_t l;
	int i;

	(void)cmd;
	(void)priv;
	if (av == NULL)
		return;
	srandom(1);
	for (i = 0; i < NRNDEXPECT; i++) {
		l = random();
		if (l == random_expect[i])
			continue;
		vtc_log(vl, 4, "random[%d] = 0x%x (expect 0x%lx)",
		    i, l, random_expect[i]);
		vtc_log(vl, 1, "SKIPPING test: unknown srandom(1) sequence.");
		vtc_stop = 1;
		break;
	}
	l = 0;
	for (i = 0; i < 1000; i++)
		l += random();
	if (l != RND_NEXT_1K) {
		vtc_log(vl, 4, "sum(random[%d...%d]) = 0x%x (expect 0x%x)",
		    NRNDEXPECT, NRNDEXPECT + 1000,
		    l, RND_NEXT_1K);
		vtc_log(vl, 1, "SKIPPING test: unknown srandom(1) sequence.");
		vtc_stop = 1;
	}
}

/**********************************************************************
 * Check features.
 */

static void
cmd_feature(CMD_ARGS)
{
	int i;

	(void)priv;
	(void)cmd;
	if (av == NULL)
		return;

	for (i = 1; av[i] != NULL; i++) {
#ifdef SO_RCVTIMEO_WORKS
		if (!strcmp(av[i], "SO_RCVTIMEO_WORKS"))
			continue;
#endif
		if (sizeof(void*) == 8 && !strcmp(av[i], "64bit"))
			continue;

		if (!strcmp(av[i], "pcre_jit") && VRE_has_jit)
			continue;

		if (!strcmp(av[i], "!OSX")) {
#if !defined(__APPLE__) || !defined(__MACH__)
			continue;
#endif
		}
		if (!strcmp(av[i], "dns") && feature_dns)
			continue;

		if (!strcmp(av[i], "topbuild") && iflg)
			continue;

		if (!strcmp(av[i], "root") && !geteuid())
			continue;

		if (!strcmp(av[i], "user_varnish") &&
		    getpwnam("varnish") != NULL)
			continue;

		if (!strcmp(av[i], "user_vcache") &&
		    getpwnam("vcache") != NULL)
			continue;

		if (!strcmp(av[i], "group_varnish") &&
		    getgrnam("varnish") != NULL)
			continue;

		vtc_log(vl, 1, "SKIPPING test, missing feature: %s", av[i]);
		vtc_stop = 1;
		return;
	}
}

/**********************************************************************
 * Execute a file
 */

static const struct cmds cmds[] = {
	{ "server",	cmd_server },
	{ "client",	cmd_client },
	{ "varnish",	cmd_varnish },
	{ "delay",	cmd_delay },
	{ "varnishtest",cmd_varnishtest },
	{ "shell",	cmd_shell },
	{ "err_shell",	cmd_err_shell },
	{ "barrier",	cmd_barrier },
	{ "random",	cmd_random },
	{ "feature",	cmd_feature },
	{ "logexpect",	cmd_logexp },
	{ "process",	cmd_process },
	{ NULL,		NULL }
};

int
exec_file(const char *fn, const char *script, const char *tmpdir,
    char *logbuf, unsigned loglen)
{
	unsigned old_err;
	FILE *f;

	(void)signal(SIGPIPE, SIG_IGN);

	vtc_loginit(logbuf, loglen);
	vltop = vtc_logopen("top");
	AN(vltop);

	init_macro();
	init_barrier();
	init_server();

	/* Move into our tmpdir */
	AZ(chdir(tmpdir));
	macro_def(vltop, NULL, "tmpdir", "%s", tmpdir);

	/* Drop file to tell what was going on here */
	f = fopen("INFO", "w");
	AN(f);
	fprintf(f, "Test case: %s\n", fn);
	AZ(fclose(f));

	vtc_stop = 0;
	vtc_log(vltop, 1, "TEST %s starting", fn);

	vtc_thread = pthread_self();
	parse_string(script, cmds, NULL, vltop);
	old_err = vtc_error;
	vtc_stop = 1;
	vtc_log(vltop, 1, "RESETTING after %s", fn);
	reset_cmds(cmds);
	vtc_error |= old_err;

	if (vtc_error)
		vtc_log(vltop, 1, "TEST %s FAILED", fn);
	else
		vtc_log(vltop, 1, "TEST %s completed", fn);

	return (vtc_error);
}
