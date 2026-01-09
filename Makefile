#

PYTHON	?=	python3
PYTHON	?=	python

VARNISH_SRC ?= /home/phk/Varnish/trunk/varnish-cache

AWK	?=	awk

SRCS=	src/*.c \
	lib/*.c

OBJS=	src/*.o \
	lib/*.o

DEPS=	lib/*.h \
	src/*.h \
	src/tbl/*.h \
	src/teken_state.h \
	src/vtc_h2_dectbl.h

FLAGS=	-O2 -Wall -Werror

CFLAGS=  ${FLAGS}
LDFLAGS= ${FLAGS} -rdynamic
DEFINES=

INCS=	-I. \
	-Isrc \
	-Ilib \
	-I/usr/local/include \
	-pthread

LIBS=	-L/usr/local/lib \
	-lm \
	-lpcre2-8 \
	-lz \
	-ldl

#######################################################################
# target for vtest without builtin varnish support

vtest: ${DEPS} ${SRCS}

	${MAKE} \
		 `for s in $(SRCS); do echo $${s%.c}.o;done`

	${CC} \
		${LDFLAGS} \
		-o vtest \
		${INCS} \
		${OBJS} \
		${LIBS}

#######################################################################
# target for vtest with builtin varnish support (needs varnish source tree)

varnishtest:	${DEPS} ${SRCS}

	@[ -d "${VARNISH_SRC}" ] || \
		( echo "${VARNISH_SRC} directory missing" 1>&2 ; exit 2)

	${MAKE} \
		 DEFINES="-DVTEST_WITH_VTC_VARNISH -DVTEST_WITH_VTC_LOGEXPECT" \
		 `for s in $(SRCS); do echo $${s%.c}.o;done`

	${CC} \
		${LDFLAGS} \
		-o varnishtest \
		${OBJS} \
		${LIBS} \
		-L${VARNISH_SRC}/lib/libvarnishapi/.libs \
		-Wl,--rpath,${VARNISH_SRC}/lib/libvarnishapi/.libs \
		-lvarnishapi

#######################################################################
# Test target

test: vtest
	env PATH=`pwd`:${PATH} vtest tests/*.vtc

#######################################################################
# Install target.
# 1. You must set DESTDIR
# 2. DESTDIR must have 'include' and 'bin' subdirs.

install: vtest _install
	@[ ! -z "${DESTDIR}" ] || \
		( echo "You must set DESTDIR" 1>&2 ; exit 2)
	@[ -d "${DESTDIR}" ] || \
		( echo "${DESTDIR} directory missing" 1>&2 ; exit 2)
	@[ -d "${DESTDIR}/bin" ] || \
		( echo "${DESTDIR}/bin directory missing" 1>&2 ; exit 2)
	@[ -d "${DESTDIR}/include" ] || \
		( echo "${DESTDIR}/include directory missing" 1>&2 ; exit 2)

	rm -f ${DESTDIR}/bin/vtest
	cp vtest ${DESTDIR}/bin/vtest
	chmod 555 ${DESTDIR}/bin/vtest

	rm -f ${DESTDIR}/include/vtest_api.h
	cp src/vtest_api.h ${DESTDIR}/include/vtest_api.h
	chmod 444 ${DESTDIR}/include/vtest_api.h

#######################################################################
# Implicit rule used in a sub-process by the rules above, and makes use
# of ${DEFINES} for extra arguments :
.c.o:
	${CC} \
		${CFLAGS} \
		${DEFINES} \
		${INCS} \
		-I${VARNISH_SRC}/include \
		-o $@ -c $<

#######################################################################

src/vtc_h2_dectbl.h:	src/huffman_gen.py src/tbl/vhp_huffman.h
	${PYTHON} -u src/huffman_gen.py src/tbl/vhp_huffman.h > $@ 

#######################################################################

src/teken_state.h:	src/gensequences src/sequences
	${AWK} -f src/gensequences src/sequences > src/teken_state.h

#######################################################################

clean:
	rm -rf */.deps */.dirstamp
	rm -f vtest varnishtest
	rm -f src/teken_state.h
	rm -f src/vtc_h2_dectbl.h
	rm -f ${OBJS}

#######################################################################
# Housekeeping
#
.PHONY: update

update:
	tools/sync/update-code-from-vc.sh
