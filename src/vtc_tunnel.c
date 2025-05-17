/*-
 * Copyright (c) 2020 Varnish Software
 * All rights reserved.
 *
 * Author: Dridi Boukelmoune <dridi.boukelmoune@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
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

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtc.h"

#include "vtcp.h"

/* SECTION: tunnel tunnel
 *
 * The goal of a tunnel is to help control the data transfer between two
 * parties, for example to trigger socket timeouts in the middle of protocol
 * frames, without the need to change how both parties are implemented.
 *
 * A tunnel accepts a connection and then connects on behalf of the source to
 * the desired destination. Once both connections are established the tunnel
 * will transfer bytes unchanged between the source and destination. Transfer
 * can be interrupted, usually with the help of synchronization methods like
 * barriers. Once the transfer is paused, it is possible to let a specific
 * amount of bytes move in either direction.
 *
 * SECTION: tunnel.args Arguments
 *
 * \-start
 *        Start the tunnel in background, processing the last given
 *        specification.
 *
 * \-wait
 *        Block until the thread finishes.
 *
 * \-listen STRING
 *        Dictate the listening socket for the server. STRING is of the form
 *        "IP PORT", or "HOST PORT".
 *
 *        Listens by defaults to a local random port.
 *
 * \-connect STRING
 *        Indicate the server to connect to. STRING is also of the form
 *        "IP PORT", or "HOST PORT".
 *
 *        Connects by default to a varnish instance called ``v1``.
 *
 * SECTION: tunnel.spec Specification
 *
 * The specification contains a list of tunnel commands that can be combined
 * with barriers and delays. For example::
 *
 *     tunnel t1 {
 *         barrier b1 sync
 *         pause
 *         delay 1
 *         send 42
 *         barrier b2 sync
 *         resume
 *     } -start
 *
 * If one end of the tunnel is closed before the end of the specification
 * the test case will fail. A specification that ends in a paused state will
 * implicitely resume the tunnel.
 */

enum tunnel_state_e {
	TUNNEL_ACCEPT,
	TUNNEL_RUNNING,
	TUNNEL_PAUSED,
	TUNNEL_SPEC_DONE,
	TUNNEL_POLL_DONE,
	TUNNEL_STOPPED,
};

struct tunnel_lane {
	char			buf[1024];
	ssize_t			buf_len;
	size_t			wrk_len;
	int			*rfd;
	int			*wfd;
};

struct tunnel {
	unsigned		magic;
#define TUNNEL_MAGIC		0x55286619
	char			*name;
	struct vtclog		*vl;
	VTAILQ_ENTRY(tunnel)	list;
	enum tunnel_state_e	state;

	char			*spec;

	char			connect[256];
	int			csock;
	const char		*caddr;

	char			listen[256];
	int			lsock;
	char			laddr[32];
	char			lport[32];

	int			asock;

	struct tunnel_lane	send_lane[1];
	struct tunnel_lane	recv_lane[1];

	pthread_mutex_t		mtx;		/* state and lanes->*_len */
	pthread_cond_t		cond;
	pthread_t		tspec;
	pthread_t		tpoll;
};

static pthread_mutex_t		tunnel_mtx;

static VTAILQ_HEAD(, tunnel)	tunnels = VTAILQ_HEAD_INITIALIZER(tunnels);

/**********************************************************************
 * Is the tunnel still operating?
 */

static unsigned
tunnel_is_open(struct tunnel *t)
{
	unsigned is_open;

	AZ(pthread_mutex_lock(&t->mtx));
	is_open = (t->send_lane->buf_len >= 0 && t->recv_lane->buf_len >= 0);
	AZ(pthread_mutex_unlock(&t->mtx));
	return (is_open);
}

/**********************************************************************
 * SECTION: tunnel.spec.pause
 *
 * pause
 *         Wait for in-flight bytes to be transferred and pause the tunnel.
 *
 *         The tunnel must be running.
 */

static void
cmd_tunnel_pause(CMD_ARGS)
{
	struct tunnel *t;

	CAST_OBJ_NOTNULL(t, priv, TUNNEL_MAGIC);
	AZ(av[1]);

	if (!tunnel_is_open(t))
		vtc_fatal(vl, "Tunnel already closed");

	AZ(pthread_mutex_lock(&t->mtx));
	if (t->state == TUNNEL_PAUSED) {
		AZ(pthread_mutex_unlock(&t->mtx));
		vtc_fatal(vl, "Tunnel already paused");
		WRONG("unreachable");
	}
	assert(t->state == TUNNEL_RUNNING);
	t->state = TUNNEL_PAUSED;
	AZ(pthread_cond_signal(&t->cond));
	AZ(pthread_cond_wait(&t->cond, &t->mtx));
	AZ(pthread_mutex_unlock(&t->mtx));
}

/**********************************************************************
 * SECTION: tunnel.spec.send
 *
 * send NUMBER
 *         Wait until NUMBER bytes are transferred from source to
 *         destination.
 *
 *         The tunnel must be paused, it remains paused afterwards.
 */

static void
cmd_tunnel_send(CMD_ARGS)
{
	struct tunnel *t;
	unsigned len;

	CAST_OBJ_NOTNULL(t, priv, TUNNEL_MAGIC);
	AN(av[1]);
	AZ(av[2]);

	len = atoi(av[1]);

	if (!tunnel_is_open(t))
		vtc_fatal(vl, "Tunnel already closed");

	AZ(pthread_mutex_lock(&t->mtx));
	if (t->state == TUNNEL_RUNNING) {
		AZ(pthread_mutex_unlock(&t->mtx));
		vtc_fatal(vl, "Tunnel still running");
		WRONG("unreachable");
	}
	assert(t->state == TUNNEL_PAUSED);
	AZ(t->send_lane->wrk_len);
	AZ(t->recv_lane->wrk_len);
	if (!strcmp(av[0], "send"))
		t->send_lane->wrk_len = len;
	else
		t->recv_lane->wrk_len = len;
	AZ(pthread_cond_signal(&t->cond));
	AZ(pthread_cond_wait(&t->cond, &t->mtx));
	AZ(pthread_mutex_unlock(&t->mtx));
}

/**********************************************************************
 * SECTION: tunnel.spec.recv
 *
 * recv NUMBER
 *         Wait until NUMBER bytes are transferred from destination to
 *         source.
 *
 *         The tunnel must be paused, it remains paused afterwards.
 */

static void
cmd_tunnel_recv(CMD_ARGS)
{

	cmd_tunnel_send(av, priv, vl);
}

/**********************************************************************
 * SECTION: tunnel.spec.resume
 *
 * resume
 *         Resume the transfer of bytes in both directions.
 *
 *         The tunnel must be paused.
 */

static void
cmd_tunnel_resume(CMD_ARGS)
{
	struct tunnel *t;

	CAST_OBJ_NOTNULL(t, priv, TUNNEL_MAGIC);
	AZ(av[1]);

	if (!tunnel_is_open(t))
		vtc_fatal(vl, "Tunnel already closed");

	AZ(pthread_mutex_lock(&t->mtx));
	if (t->state == TUNNEL_RUNNING) {
		AZ(pthread_mutex_unlock(&t->mtx));
		vtc_fatal(vl, "Tunnel already running");
		WRONG("unreachable");
	}
	assert(t->state == TUNNEL_PAUSED);
	t->state = TUNNEL_RUNNING;
	AZ(pthread_cond_signal(&t->cond));
	AZ(pthread_mutex_unlock(&t->mtx));
}

const struct cmds tunnel_cmds[] = {
#define CMD_TUNNEL(n) { #n, cmd_tunnel_##n },
	CMD_TUNNEL(pause)
	CMD_TUNNEL(send)
	CMD_TUNNEL(recv)
	CMD_TUNNEL(resume)
#undef CMD_TUNNEL
	{ NULL, NULL }
};

/**********************************************************************
 * Tunnel poll thread
 */

static void
tunnel_read(struct tunnel *t, struct vtclog *vl, struct pollfd *pfd,
    struct tunnel_lane *lane)
{
	size_t len;
	ssize_t res;
	enum tunnel_state_e state;

	assert(pfd->fd == *lane->rfd);
	if (!(pfd->revents & POLLIN))
		return;

	AZ(pthread_mutex_lock(&t->mtx));
	AZ(lane->buf_len);
	len = lane->wrk_len;
	state = t->state;
	AZ(pthread_mutex_unlock(&t->mtx));

	if (len == 0 && state == TUNNEL_PAUSED)
		return;

	if (len == 0 || len > sizeof lane->buf)
		len = sizeof lane->buf;

	res = read(pfd->fd, lane->buf, len);
	if (res < 0)
		vtc_fatal(vl, "Read failed: %s", strerror(errno));

	AZ(pthread_mutex_lock(&t->mtx));
	lane->buf_len = (res == 0) ? -1 : res;
	AZ(pthread_mutex_unlock(&t->mtx));
}

static void
tunnel_write(struct tunnel *t, struct vtclog *vl, struct tunnel_lane *lane,
    const char *action)
{
	const char *p;
	ssize_t res, l;

	p = lane->buf;
	AZ(pthread_mutex_lock(&t->mtx));
	l = lane->buf_len;
	AZ(pthread_mutex_unlock(&t->mtx));

	if (l > 0)
		vtc_log(vl, 3, "%s %zd bytes", action, l);
	while (l > 0) {
		res = write(*lane->wfd, p, l);
		if (res <= 0)
			vtc_fatal(vl, "Write failed: %s", strerror(errno));
		l -= res;
		p += res;
	}

	AZ(pthread_mutex_lock(&t->mtx));
	if (lane->wrk_len > 0 && lane->buf_len != -1) {
		assert(lane->buf_len >= 0);
		assert(lane->wrk_len >= lane->buf_len);
		lane->wrk_len -= lane->buf_len;
	}
	lane->buf_len = l;
	AZ(pthread_mutex_unlock(&t->mtx));
}

static void *
tunnel_poll_thread(void *priv)
{
	struct tunnel *t;
	struct vtclog *vl;
	struct pollfd pfd[2];
	enum tunnel_state_e state;
	int res;

	CAST_OBJ_NOTNULL(t, priv, TUNNEL_MAGIC);

	vl = vtc_logopen("%s", t->name);
	pthread_cleanup_push(vtc_logclose, vl);

	while (tunnel_is_open(t) && !vtc_stop) {
		AZ(pthread_mutex_lock(&t->mtx));
		/* NB: can be woken up by `tunnel tX -wait` */
		while (t->state == TUNNEL_ACCEPT)
			AZ(pthread_cond_wait(&t->cond, &t->mtx));
		state = t->state;
		AZ(pthread_mutex_unlock(&t->mtx));

		assert(state < TUNNEL_POLL_DONE);

		memset(pfd, 0, sizeof pfd);
		pfd[0].fd = *t->send_lane->rfd;
		pfd[1].fd = *t->recv_lane->rfd;
		pfd[0].events = POLLIN;
		pfd[1].events = POLLIN;
		res = poll(pfd, 2, 100);
		if (res == -1)
			vtc_fatal(vl, "Poll failed: %s", strerror(errno));

		tunnel_read(t, vl, &pfd[0], t->send_lane);
		tunnel_read(t, vl, &pfd[1], t->recv_lane);

		AZ(pthread_mutex_lock(&t->mtx));
		if (t->state == TUNNEL_PAUSED && t->send_lane->wrk_len == 0 &&
		    t->recv_lane->wrk_len == 0) {
			AZ(t->send_lane->buf_len);
			AZ(t->recv_lane->buf_len);
			AZ(pthread_cond_signal(&t->cond));
			AZ(pthread_cond_wait(&t->cond, &t->mtx));
		}
		AZ(pthread_mutex_unlock(&t->mtx));

		tunnel_write(t, vl, t->send_lane, "Sending");
		tunnel_write(t, vl, t->recv_lane, "Receiving");
	}

	AZ(pthread_mutex_lock(&t->mtx));
	if (t->state != TUNNEL_SPEC_DONE && !vtc_stop) {
		AZ(pthread_cond_signal(&t->cond));
		AZ(pthread_cond_wait(&t->cond, &t->mtx));
	}
	AZ(pthread_mutex_unlock(&t->mtx));

	pthread_cleanup_pop(0);
	vtc_logclose(vl);
	t->state = TUNNEL_POLL_DONE;
	return (NULL);
}

/**********************************************************************
 * Tunnel spec thread
 */

static void
tunnel_accept(struct tunnel *t, struct vtclog *vl)
{
	struct vsb *vsb;
	const char *addr, *err;
	int afd, cfd;

	CHECK_OBJ_NOTNULL(t, TUNNEL_MAGIC);
	assert(t->lsock >= 0);
	assert(t->asock < 0);
	assert(t->csock < 0);
	assert(t->state == TUNNEL_ACCEPT);

	vtc_log(vl, 4, "Accepting");
	afd = accept(t->lsock, NULL, NULL);
	if (afd < 0)
		vtc_fatal(vl, "Accept failed: %s", strerror(errno));
	vtc_log(vl, 3, "Accepted socket fd is %d", afd);

	vsb = macro_expand(vl, t->connect);
	AN(vsb);
	addr = VSB_data(vsb);

	cfd = VTCP_open(addr, NULL, 10., &err);
	if (cfd < 0)
		vtc_fatal(vl, "Failed to open %s: %s", addr, err);
	vtc_log(vl, 3, "Connected socket fd is %d", cfd);
	VSB_destroy(&vsb);

	VTCP_blocking(afd);
	VTCP_blocking(cfd);

	AZ(pthread_mutex_lock(&t->mtx));
	t->asock = afd;
	t->csock = cfd;
	t->send_lane->buf_len = 0;
	t->send_lane->wrk_len = 0;
	t->recv_lane->buf_len = 0;
	t->recv_lane->wrk_len = 0;
	t->state = TUNNEL_RUNNING;
	AZ(pthread_cond_signal(&t->cond));
	AZ(pthread_mutex_unlock(&t->mtx));
}

static void *
tunnel_spec_thread(void *priv)
{
	struct tunnel *t;
	struct vtclog *vl;
	enum tunnel_state_e state;

	CAST_OBJ_NOTNULL(t, priv, TUNNEL_MAGIC);
	AN(*t->connect);

	vl = vtc_logopen("%s", t->name);
	vtc_log_set_cmd(vl, tunnel_cmds);
	pthread_cleanup_push(vtc_logclose, vl);

	tunnel_accept(t, vl);
	parse_string(vl, t, t->spec);

	AZ(pthread_mutex_lock(&t->mtx));
	state = t->state;
	AZ(pthread_mutex_unlock(&t->mtx));

	if (state == TUNNEL_PAUSED && !vtc_stop)
		parse_string(vl, t, "resume");

	AZ(pthread_mutex_lock(&t->mtx));
	t->state = TUNNEL_SPEC_DONE;
	AZ(pthread_cond_signal(&t->cond));
	AZ(pthread_mutex_unlock(&t->mtx));

	vtc_log(vl, 2, "Ending");
	pthread_cleanup_pop(0);
	vtc_logclose(vl);
	return (NULL);
}

/**********************************************************************
 * Tunnel management
 */

static struct tunnel *
tunnel_new(const char *name)
{
	struct tunnel *t;

	ALLOC_OBJ(t, TUNNEL_MAGIC);
	AN(t);
	REPLACE(t->name, name);
	t->vl = vtc_logopen("%s", name);
	AN(t->vl);

	t->state = TUNNEL_STOPPED;
	bprintf(t->connect, "%s", "${v1_sock}");
	bprintf(t->listen, "%s", "127.0.0.1 0");
	t->csock = -1;
	t->lsock = -1;
	t->asock = -1;
	t->send_lane->rfd = &t->asock;
	t->send_lane->wfd = &t->csock;
	t->recv_lane->rfd = &t->csock;
	t->recv_lane->wfd = &t->asock;
	AZ(pthread_mutex_init(&t->mtx, NULL));
	AZ(pthread_cond_init(&t->cond, NULL));
	AZ(pthread_mutex_lock(&tunnel_mtx));
	VTAILQ_INSERT_TAIL(&tunnels, t, list);
	AZ(pthread_mutex_unlock(&tunnel_mtx));
	return (t);
}

static void
tunnel_delete(struct tunnel *t)
{

	CHECK_OBJ_NOTNULL(t, TUNNEL_MAGIC);
	assert(t->asock < 0);
	assert(t->csock < 0);
	if (t->lsock >= 0)
		VTCP_close(&t->lsock);
	macro_undef(t->vl, t->name, "addr");
	macro_undef(t->vl, t->name, "port");
	macro_undef(t->vl, t->name, "sock");
	vtc_logclose(t->vl);
	(void)pthread_mutex_destroy(&t->mtx);
	(void)pthread_cond_destroy(&t->cond);
	free(t->name);
	FREE_OBJ(t);
}

/**********************************************************************
 * Tunnel listen
 */

static void
tunnel_listen(struct tunnel *t)
{
	const char *err;

	if (t->lsock >= 0)
		VTCP_close(&t->lsock);
	t->lsock = VTCP_listen_on(t->listen, "0", 1, &err);
	if (err != NULL)
		vtc_fatal(t->vl,
		    "Tunnel listen address (%s) cannot be resolved: %s",
		    t->listen, err);
	assert(t->lsock > 0);
	VTCP_myname(t->lsock, t->laddr, sizeof t->laddr,
	    t->lport, sizeof t->lport);
	macro_def(t->vl, t->name, "addr", "%s", t->laddr);
	macro_def(t->vl, t->name, "port", "%s", t->lport);
	macro_def(t->vl, t->name, "sock", "%s %s", t->laddr, t->lport);
	/* Record the actual port, and reuse it on subsequent starts */
	bprintf(t->listen, "%s %s", t->laddr, t->lport);
}

/**********************************************************************
 * Start the tunnel thread
 */

static void
tunnel_start(struct tunnel *t)
{

	CHECK_OBJ_NOTNULL(t, TUNNEL_MAGIC);
	vtc_log(t->vl, 2, "Starting tunnel");
	tunnel_listen(t);
	vtc_log(t->vl, 1, "Listen on %s", t->listen);
	assert(t->state == TUNNEL_STOPPED);
	t->state = TUNNEL_ACCEPT;
	t->send_lane->buf_len = 0;
	t->send_lane->wrk_len = 0;
	t->recv_lane->buf_len = 0;
	t->recv_lane->wrk_len = 0;
	AZ(pthread_create(&t->tpoll, NULL, tunnel_poll_thread, t));
	AZ(pthread_create(&t->tspec, NULL, tunnel_spec_thread, t));
}

/**********************************************************************
 * Wait for tunnel thread to stop
 */

static void
tunnel_wait(struct tunnel *t)
{
	void *res;

	CHECK_OBJ_NOTNULL(t, TUNNEL_MAGIC);
	vtc_log(t->vl, 2, "Waiting for tunnel");

	AZ(pthread_cond_signal(&t->cond));

	AZ(pthread_join(t->tspec, &res));
	if (res == PTHREAD_CANCELED && !vtc_stop)
		vtc_fatal(t->vl, "Tunnel spec canceled");
	if (res != NULL && !vtc_stop)
		vtc_fatal(t->vl, "Tunnel spec returned \"%p\"", res);

	AZ(pthread_join(t->tpoll, &res));
	if (res == PTHREAD_CANCELED && !vtc_stop)
		vtc_fatal(t->vl, "Tunnel poll canceled");
	if (res != NULL && !vtc_stop)
		vtc_fatal(t->vl, "Tunnel poll returned \"%p\"", res);

	if (t->csock >= 0)
		VTCP_close(&t->csock);
	if (t->asock >= 0)
		VTCP_close(&t->asock);
	t->tspec = 0;
	t->tpoll = 0;
	t->state = TUNNEL_STOPPED;
}

/**********************************************************************
 * Reap tunnel
 */

static void
tunnel_reset(void)
{
	struct tunnel *t;
	enum tunnel_state_e state;

	while (1) {
		AZ(pthread_mutex_lock(&tunnel_mtx));
		t = VTAILQ_FIRST(&tunnels);
		CHECK_OBJ_ORNULL(t, TUNNEL_MAGIC);
		if (t != NULL)
			VTAILQ_REMOVE(&tunnels, t, list);
		AZ(pthread_mutex_unlock(&tunnel_mtx));
		if (t == NULL)
			break;

		AZ(pthread_mutex_lock(&t->mtx));
		state = t->state;
		if (state < TUNNEL_POLL_DONE)
			(void)pthread_cancel(t->tpoll);
		if (state < TUNNEL_SPEC_DONE)
			(void)pthread_cancel(t->tspec);
		AZ(pthread_mutex_unlock(&t->mtx));

		if (state != TUNNEL_STOPPED)
			tunnel_wait(t);
		tunnel_delete(t);
	}
}

/**********************************************************************
 * Tunnel command dispatch
 */

void
cmd_tunnel(CMD_ARGS)
{
	struct tunnel *t;

	(void)priv;

	if (av == NULL) {
		/* Reset and free */
		tunnel_reset();
		return;
	}

	AZ(strcmp(av[0], "tunnel"));
	av++;

	VTC_CHECK_NAME(vl, av[0], "Tunnel", 't');

	AZ(pthread_mutex_lock(&tunnel_mtx));
	VTAILQ_FOREACH(t, &tunnels, list)
		if (!strcmp(t->name, av[0]))
			break;
	AZ(pthread_mutex_unlock(&tunnel_mtx));
	if (t == NULL)
		t = tunnel_new(av[0]);
	CHECK_OBJ_NOTNULL(t, TUNNEL_MAGIC);
	av++;

	for (; *av != NULL; av++) {
		if (vtc_error)
			break;
		if (!strcmp(*av, "-wait")) {
			if (t->state == TUNNEL_STOPPED)
				vtc_fatal(t->vl, "Tunnel not -started");
			tunnel_wait(t);
			continue;
		}

		/* Don't mess with a running tunnel */
		if (t->state != TUNNEL_STOPPED)
			tunnel_wait(t);

		assert(t->state == TUNNEL_STOPPED);
		if (!strcmp(*av, "-connect")) {
			bprintf(t->connect, "%s", av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-listen")) {
			bprintf(t->listen, "%s", av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-start")) {
			tunnel_start(t);
			continue;
		}
		if (**av == '-')
			vtc_fatal(t->vl, "Unknown tunnel argument: %s", *av);
		t->spec = *av;
	}
}

void
init_tunnel(void)
{

	AZ(pthread_mutex_init(&tunnel_mtx, NULL));
}
