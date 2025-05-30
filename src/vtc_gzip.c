/*-
 * Copyright (c) 2008-2019 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vtc.h"
#include "vtc_http.h"
#if defined(WITH_ZLIB)
#include <zlib.h>
#else
#include "vgz.h"
#endif

#ifdef VGZ_EXTENSIONS
static void
vtc_report_gz_bits(struct vtclog *vl, const z_stream *vz)
{
	vtc_log(vl, 4, "startbit = %ju %ju/%ju",
	    (uintmax_t)vz->start_bit,
	    (uintmax_t)vz->start_bit >> 3, (uintmax_t)vz->start_bit & 7);
	vtc_log(vl, 4, "lastbit = %ju %ju/%ju",
	    (uintmax_t)vz->last_bit,
	    (uintmax_t)vz->last_bit >> 3, (uintmax_t)vz->last_bit & 7);
	vtc_log(vl, 4, "stopbit = %ju %ju/%ju",
	    (uintmax_t)vz->stop_bit,
	    (uintmax_t)vz->stop_bit >> 3, (uintmax_t)vz->stop_bit & 7);
}
#endif

static size_t
APOS(ssize_t sz)
{
	assert(sz >= 0);
	return (sz);
}

/**********************************************************************
 * GUNZIPery
 */

static struct vsb *
vtc_gunzip_vsb(struct vtclog *vl, int fatal, const struct vsb *vin)
{
	z_stream vz;
	struct vsb *vout;
	int i;
	char buf[BUFSIZ];

	memset(&vz, 0, sizeof vz);
	vout = VSB_new_auto();
	AN(vout);

	vz.next_in = (void*)VSB_data(vin);
	vz.avail_in = APOS(VSB_len(vin));

	assert(Z_OK == inflateInit2(&vz, 31));

	do {
		vz.next_out = (void*)buf;
		vz.avail_out = sizeof buf;
		i = inflate(&vz, Z_FINISH);
		if (vz.avail_out != sizeof buf)
			VSB_bcat(vout, buf, sizeof buf - vz.avail_out);
	} while (i == Z_OK || i == Z_BUF_ERROR);
	if (i != Z_STREAM_END)
		vtc_log(vl, fatal,
		    "Gunzip error = %d (%s) in:%jd out:%jd len:%zd",
		    i, vz.msg, (intmax_t)vz.total_in, (intmax_t)vz.total_out,
		    VSB_len(vin));
	AZ(VSB_finish(vout));
#ifdef VGZ_EXTENSIONS
	vtc_report_gz_bits(vl, &vz);
#endif
	assert(Z_OK == inflateEnd(&vz));
	return (vout);
}

void
vtc_gunzip(struct http *hp, char *body, long *bodylen)
{
	struct vsb *vin, *vout;

	AN(body);
	if (body[0] != (char)0x1f || body[1] != (char)0x8b)
		vtc_log(hp->vl, hp->fatal,
		    "Gunzip error: body lacks gzip magic");

	vin = VSB_new_auto();
	AN(vin);
	VSB_bcat(vin, body, *bodylen);
	AZ(VSB_finish(vin));
	vout = vtc_gunzip_vsb(hp->vl, hp->fatal, vin);
	VSB_destroy(&vin);

	memcpy(body, VSB_data(vout), APOS(VSB_len(vout) + 1));
	*bodylen = APOS(VSB_len(vout));
	VSB_destroy(&vout);
	vtc_log(hp->vl, 3, "new bodylen %ld", *bodylen);
	vtc_dump(hp->vl, 4, "body", body, *bodylen);
	bprintf(hp->bodylen, "%ld", *bodylen);
}

/**********************************************************************
 * GZIPery
 */

static int
vtc_gzip_chunk(z_stream *vz, struct vsb *vout, const char *in, size_t inlen, int flush)
{
	int i;
	char buf[BUFSIZ];

	vz->next_in = TRUST_ME(in);
	vz->avail_in = APOS(inlen);
	do {
		vz->next_out = (void*)buf;
		vz->avail_out = sizeof buf;
		i = deflate(vz, flush);
		if (vz->avail_out != sizeof buf)
			VSB_bcat(vout, buf, sizeof buf - vz->avail_out);
	} while (i == Z_OK || vz->avail_in > 0);
	vz->next_out = NULL;
	vz->avail_out = 0;
	vz->next_in = NULL;
	AZ(vz->avail_in);
	vz->avail_in = 0;
	return (i);
}

static void
vtc_gzip(struct http *hp, const char *input, char **body, long *bodylen, int fragment)
{
	struct vsb *vout;
	int i, res;
	size_t inlen = strlen(input);
	z_stream vz;

	memset(&vz, 0, sizeof vz);
	vout = VSB_new_auto();
	AN(vout);

	assert(Z_OK == deflateInit2(&vz,
	    hp->gziplevel, Z_DEFLATED, 31, 9, Z_DEFAULT_STRATEGY));

	while (fragment && inlen > 3) {
		res = inlen / 3;
		i = vtc_gzip_chunk(&vz, vout, input, res, Z_BLOCK);
		if (i != Z_OK && i != Z_BUF_ERROR) {
			vtc_log(hp->vl, hp->fatal,
			    "Gzip error = %d (%s) in:%jd out:%jd len:%zd",
			    i, vz.msg, (intmax_t)vz.total_in,
			    (intmax_t)vz.total_out, strlen(input));
		}
		input += res;
		inlen -= res;
	}

	i = vtc_gzip_chunk(&vz, vout, input, inlen, Z_FINISH);
	if (i != Z_STREAM_END) {
		vtc_log(hp->vl, hp->fatal,
		    "Gzip error = %d (%s) in:%jd out:%jd len:%zd",
		    i, vz.msg, (intmax_t)vz.total_in, (intmax_t)vz.total_out,
		    strlen(input));
	}
	AZ(VSB_finish(vout));
#ifdef VGZ_EXTENSIONS
	res = vz.stop_bit & 7;
	vtc_report_gz_bits(hp->vl, &vz);
#else
	res = 0;
#endif
	assert(Z_OK == deflateEnd(&vz));

#ifdef VGZ_EXTENSIONS
	if (hp->gzipresidual >= 0 && hp->gzipresidual != res)
		vtc_log(hp->vl, hp->fatal,
		    "Wrong gzip residual got %d wanted %d",
		    res, hp->gzipresidual);
#endif
	*body = malloc(APOS(VSB_len(vout) + 1));
	AN(*body);
	memcpy(*body, VSB_data(vout), APOS(VSB_len(vout) + 1));
	*bodylen = APOS(VSB_len(vout));
	VSB_destroy(&vout);
	vtc_log(hp->vl, 3, "new bodylen %ld", *bodylen);
	vtc_dump(hp->vl, 4, "body", *body, *bodylen);
	bprintf(hp->bodylen, "%ld", *bodylen);
}

int
vtc_gzip_cmd(struct http *hp, char * const *av, char **body, long *bodylen)
{
	char *b;

	AN(hp);
	AN(av);
	AN(body);
	AN(bodylen);

	if (!strcmp(*av, "-gzipresidual")) {
		hp->gzipresidual = strtoul(av[1], NULL, 0);
		return (1);
	}
	if (!strcmp(*av, "-gziplevel")) {
		hp->gziplevel = strtoul(av[1], NULL, 0);
		return (1);
	}
	if (!strcmp(*av, "-gzipbody")) {
		if (*body != NULL)
			free(*body);
		*body = NULL;
		vtc_gzip(hp, av[1], body, bodylen, 0);
		AN(*body);
		return (2);
	}
	if (!strcmp(*av, "-gziplen")) {
		if (*body != NULL)
			free(*body);
		*body = NULL;
		b = synth_body(av[1], 1);
		vtc_gzip(hp, b, body, bodylen, 1);
		AN(*body);
		free(b);
		return (2);
	}
	return (0);
}
