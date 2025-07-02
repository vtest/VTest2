# comments explain how this file was prepared
# 
# lib:
# - start with list of files currently in vtest/lib
# - find then in the varnish-cache tree
#   for f in $(cat /tmp/lib) ; do find . -name $f ; done | tee /tmp/f
#   this has one too many ./tools/coccinelle/vdef.h (manually removed)
# - for those, create the list
#   <file>
#   <file>==>lib/<basename>
# - in addition, find all renames:
#   for f in $(cat /tmp/f) ; do echo ; echo $f ; echo ---------- ; git log --name-status --diff-filter=R --follow -- $f ; done | awk '/^R/ { print $2 }'
# - manually check the renames
#   for f in $(cat /tmp/f) ; do echo ; echo $f ; echo ---------- ; git log --oneline --name-only --follow -- $f ; done
include/miniobj.h
include/miniobj.h==>lib/miniobj.h
lib/libvarnish/vas.c
lib/libvarnish/vas.c==>lib/vas.c
include/vas.h
include/vas.h==>lib/vas.h
lib/libvarnish/vav.c
lib/libvarnish/vav.c==>lib/vav.c
include/vav.h
include/vav.h==>lib/vav.h
lib/libvarnish/vbh.c
lib/libvarnish/vbh.c==>lib/vbh.c
include/vbh.h
include/vbh.h==>lib/vbh.h
lib/libvarnish/vbt.c
lib/libvarnish/vbt.c==>lib/vbt.c
include/vbt.h
include/vbt.h==>lib/vbt.h
include/vcli.h
include/vcli.h==>lib/vcli.h
lib/libvarnish/vct.c
lib/libvarnish/vct.c==>lib/vct.c
include/vct.h
include/vct.h==>lib/vct.h
include/vdef.h
include/vdef.h==>lib/vdef.h
include/vend.h
include/vend.h==>lib/vend.h
lib/libvarnish/vev.c
lib/libvarnish/vev.c==>lib/vev.c
include/vev.h
include/vev.h==>lib/vev.h
lib/libvarnish/vfil.c
lib/libvarnish/vfil.c==>lib/vfil.c
include/vfil.h
include/vfil.h==>lib/vfil.h
lib/libvarnish/vfl.c
lib/libvarnish/vfl.c==>lib/vfl.c
include/vfl.h
include/vfl.h==>lib/vfl.h
lib/libvgz/vgz.h
lib/libvgz/vgz.h==>lib/vgz.h
lib/libvarnish/vjsn.c
lib/libvarnish/vjsn.c==>lib/vjsn.c
include/vjsn.h
include/vjsn.h==>lib/vjsn.h
lib/libvarnish/vlu.c
lib/libvarnish/vlu.c==>lib/vlu.c
include/vlu.h
include/vlu.h==>lib/vlu.h
lib/libvarnish/vnum.c
lib/libvarnish/vnum.c==>lib/vnum.c
include/vnum.h
include/vnum.h==>lib/vnum.h
lib/libvarnish/vpf.c
lib/libvarnish/vpf.c==>lib/vpf.c
include/vpf.h
include/vpf.h==>lib/vpf.h
include/vqueue.h
include/vqueue.h==>lib/vqueue.h
lib/libvarnish/vre.c
lib/libvarnish/vre.c==>lib/vre.c
include/vre.h
include/vre.h==>lib/vre.h
include/vre_pcre2.h
include/vre_pcre2.h==>lib/vre_pcre2.h
lib/libvarnish/vrnd.c
lib/libvarnish/vrnd.c==>lib/vrnd.c
include/vrnd.h
include/vrnd.h==>lib/vrnd.h
lib/libvarnish/vsa.c
lib/libvarnish/vsa.c==>lib/vsa.c
include/vsa.h
include/vsa.h==>lib/vsa.h
lib/libvarnish/vsb.c
lib/libvarnish/vsb.c==>lib/vsb.c
include/vsb.h
include/vsb.h==>lib/vsb.h
include/vapi/vsig.h
include/vapi/vsig.h==>lib/vapi/vsig.h
lib/libvarnish/vss.c
lib/libvarnish/vss.c==>lib/vss.c
include/vss.h
include/vss.h==>lib/vss.h
lib/libvarnish/vsub.c
lib/libvarnish/vsub.c==>lib/vsub.c
include/vsub.h
include/vsub.h==>lib/vsub.h
lib/libvarnish/vtcp.c
lib/libvarnish/vtcp.c==>lib/vtcp.c
include/vtcp.h
include/vtcp.h==>lib/vtcp.h
lib/libvarnish/vtim.c
lib/libvarnish/vtim.c==>lib/vtim.c
include/vtim.h
include/vtim.h==>lib/vtim.h
lib/libvarnish/vus.c
lib/libvarnish/vus.c==>lib/vus.c
include/vus.h
include/vus.h==>lib/vus.h
# previous names of files in lib/ (see top comment)
# manually removed because of failed git copy heuristic:
# - bin/varnishd/cache_acceptor.h
# manually removed because "too much history"
# - include/libvarnish.h
lib/libvarnish/assert.c
lib/libvarnish/argv.c
include/argv.h
include/cli.h
lib/libvarnish/vtmpfile.c
lib/libvarnish/flopen.c
include/flopen.h
lib/libvgz/zlib.h
lib/libvarnishapi/vjsn.c
lib/libvarnishapi/vjsn.h
lib/libvarnish/num.c
lib/libvarnishcompat/srandomdev.c
lib/libcompat/srandomdev.c
include/sbuf.h
lib/libvarnish/subproc.c
lib/libvarnish/tcp.c
bin/varnishd/tcp.c
lib/libvarnish/time.c
lib/libvarnishapi/instance.c
# src/tbl
include/tbl/h2_error.h
include/tbl/h2_error.h==>src/tbl/h2_error.h
include/tbl/h2_frames.h
include/tbl/h2_frames.h==>src/tbl/h2_frames.h
include/tbl/h2_settings.h
include/tbl/h2_settings.h==>src/tbl/h2_settings.h
include/tbl/h2_stream.h
include/tbl/h2_stream.h==>src/tbl/h2_stream.h
include/tbl/vhp_huffman.h
include/tbl/vhp_huffman.h==>src/tbl/vhp_huffman.h
include/tbl/vsig_list.h
include/tbl/vsig_list.h==>src/tbl/vsig_list.h
