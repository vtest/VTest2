/*-
 * Copyright (c) 2008-2025 Varnish Software AS
 * All rights reserved.
 *
 * Author: Frédéric Lécaille <flecaille@haproxy.com>
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

#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtc.h"

#include "vas.h"
#include "vsa.h"
#include "vss.h"
#include "vudp.h"

/*--------------------------------------------------------------------
 * Check if a UDP syscall return value is fatal
 * XXX: Largely copied from VTCP, not sure if really applicable
 */

int
VUDP_Check(int a)
{
	if (a == 0)
		return (1);
	if (errno == ECONNRESET)
		return (1);
#if (defined (__SVR4) && defined (__sun)) || defined (__NetBSD__)
	/*
	 * Solaris returns EINVAL if the other end unexpectedly reset the
	 * connection.
	 * This is a bug in Solaris and documented behaviour on NetBSD.
	 */
	if (errno == EINVAL || errno == ETIMEDOUT || errno == EPIPE)
		return (1);
#elif defined (__APPLE__)
	/*
	 * macOS returns EINVAL if the other end unexpectedly reset
	 * the connection.
	 */
	if (errno == EINVAL)
		return (1);
#endif
	return (0);
}

/*--------------------------------------------------------------------
 * When closing a UDP connection, a couple of errno's are legit, we
 * can't be held responsible for the other end wanting to talk to us.
 */

void
VUDP_close(int *s)
{
	int i;

	i = close(*s);

	assert(VUDP_Check(i));
	*s = -1;
}

/*--------------------------------------------------------------------
 * Given a struct suckaddr, open a socket of the appropriate type, and bind
 * it to the requested address.
 *
 * If the address is an IPv6 address, the IPV6_V6ONLY option is set to
 * avoid conflicts between INADDR_ANY and IN6ADDR_ANY.
 */

int
VUDP_bind(const struct suckaddr *sa, const char **errp)
{
#ifdef IPV6_V6ONLY
	int val;
#endif
	int sd, e;
	socklen_t sl;
	const struct sockaddr *so;
	int proto;

	if (errp != NULL)
		*errp = NULL;

	proto = VSA_Get_Proto(sa);
	sd = socket(proto, SOCK_DGRAM, 0);
	if (sd < 0) {
		if (errp != NULL)
			*errp = "socket(2)";
		return (-1);
	}

#ifdef IPV6_V6ONLY
	/* forcibly use separate sockets for IPv4 and IPv6 */
	val = 1;
	if (proto == AF_INET6 &&
	    setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, &val, sizeof val) != 0) {
		if (errp != NULL)
			*errp = "setsockopt(IPV6_V6ONLY, 1)";
		e = errno;
		closefd(&sd);
		errno = e;
		return (-1);
	}
#endif
	so = VSA_Get_Sockaddr(sa, &sl);
	if (bind(sd, so, sl) != 0) {
		if (errp != NULL)
			*errp = "bind(2)";
		e = errno;
		closefd(&sd);
		errno = e;
		return (-1);
	}
	return (sd);
}

static int v_matchproto_(vss_resolved_f)
vudp_lo_cb(void *priv, const struct suckaddr *sa)
{
	int sock;
	struct udp_helper *hp = priv;

	sock = VUDP_bind(sa, hp->errp);
	if (sock > 0) {
		*hp->errp = NULL;
		return (sock);
	}
	AN(*hp->errp);
	return (0);
}

int
VUDP_bind_on(const char *addr, const char *def_port, const char **errp)
{
	struct udp_helper h;
	int sock;

	h.errp = errp;

	sock = VSS_resolver_socktype(
	    addr, def_port, vudp_lo_cb, &h, errp, SOCK_DGRAM);
	if (*errp != NULL)
		return (-1);
	return (sock);
}
