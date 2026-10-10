PROG=	unfsd

SRCS=	src/main.c src/log.c src/xdr.c src/rpc.c src/net.c \
	src/portmap.c src/nfs2.c src/nfs3.c src/mount.c \
	src/conf.c src/fh.c src/fs.c src/nfs_common.c src/drc.c src/dirlist.c src/dircache.c
OBJS=	$(SRCS:.c=.o)
HDRS=	src/log.h src/xdr.h src/rpc.h src/types.h src/net.h \
	src/portmap.h src/progs.h src/conf.h src/fh.h src/fs.h \
	src/nfs_common.h src/port.h src/drc.h src/dirlist.h src/dircache.h

CC?=	cc
UNAME_S:=$(shell uname -s)
ifeq ($(UNAME_S),Linux)
CPPFLAGS+=	-I/usr/include/tirpc
LDFLAGS+=	-ltirpc
endif
CFLAGS=	-O2 -g -Wall -Wextra -Wno-long-long \
	-Wshadow -Wpointer-arith -Wcast-qual -Wwrite-strings

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Header dependencies (per-target, to trigger rebuild on header change)
src/main.o: src/main.c $(HDRS)
src/log.o: src/log.c $(HDRS)
src/xdr.o: src/xdr.c $(HDRS)
src/rpc.o: src/rpc.c $(HDRS)
src/net.o: src/net.c $(HDRS)
src/portmap.o: src/portmap.c $(HDRS)
src/nfs2.o: src/nfs2.c $(HDRS)
src/nfs3.o: src/nfs3.c $(HDRS)
src/mount.o: src/mount.c $(HDRS)
src/conf.o: src/conf.c $(HDRS)
src/fh.o: src/fh.c $(HDRS)
src/fs.o: src/fs.c $(HDRS)
src/nfs_common.o: src/nfs_common.c $(HDRS)
src/drc.o: src/drc.c $(HDRS)
src/dirlist.o: src/dirlist.c $(HDRS)
src/dircache.o: src/dircache.c $(HDRS)

TESTS=	tests/xdr_test tests/nfsstat_test tests/fh_test tests/pathcache_test tests/drc_test tests/rpc_drc_test tests/setattr_test tests/net_idle_test tests/dirlist_test tests/readdir3_test tests/readdir2_test

tests/xdr_test: tests/xdr_test.c src/xdr.c src/xdr.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/xdr_test.c src/xdr.c

tests/nfsstat_test: tests/nfsstat_test.c src/nfs_common.c src/xdr.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/nfsstat_test.c \
	    src/nfs_common.c src/xdr.c

tests/fh_test: tests/fh_test.c src/fh.c src/fs.c src/conf.c src/log.c \
	    src/nfs_common.c src/xdr.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/fh_test.c src/fh.c \
	    src/fs.c src/conf.c src/log.c src/nfs_common.c src/xdr.c

tests/pathcache_test: tests/pathcache_test.c src/fh.c src/conf.c src/log.c \
	    src/nfs_common.c src/xdr.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/pathcache_test.c src/fh.c \
	    src/conf.c src/log.c src/nfs_common.c src/xdr.c

tests/drc_test: tests/drc_test.c src/drc.c src/drc.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/drc_test.c src/drc.c

tests/rpc_drc_test: tests/rpc_drc_test.c src/rpc.c src/drc.c src/conf.c \
	    src/log.c src/xdr.c src/nfs_common.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/rpc_drc_test.c src/rpc.c \
	    src/drc.c src/conf.c src/log.c src/xdr.c src/nfs_common.c

tests/setattr_test: tests/setattr_test.c src/fs.c src/fh.c src/conf.c \
	    src/log.c src/nfs_common.c src/xdr.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/setattr_test.c src/fs.c \
	    src/fh.c src/conf.c src/log.c src/nfs_common.c src/xdr.c

tests/net_idle_test: tests/net_idle_test.c src/net.c src/rpc.c src/drc.c \
	    src/conf.c src/log.c src/xdr.c src/nfs_common.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/net_idle_test.c src/net.c \
	    src/rpc.c src/drc.c src/conf.c src/log.c src/xdr.c src/nfs_common.c

tests/dirlist_test: tests/dirlist_test.c src/dirlist.c src/dirlist.h \
	    src/nfs_common.c src/xdr.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/dirlist_test.c \
	    src/dirlist.c src/nfs_common.c src/xdr.c

tests/readdir3_test: tests/readdir3_test.c src/nfs3.c src/fs.c src/fh.c \
	    src/conf.c src/log.c src/nfs_common.c src/xdr.c src/dirlist.c \
	    src/dircache.c src/rpc.c src/drc.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/readdir3_test.c src/nfs3.c \
	    src/fs.c src/fh.c src/conf.c src/log.c src/nfs_common.c src/xdr.c \
	    src/dirlist.c src/dircache.c src/rpc.c src/drc.c

tests/readdir2_test: tests/readdir2_test.c src/nfs2.c src/fs.c src/fh.c \
	    src/conf.c src/log.c src/nfs_common.c src/xdr.c src/dirlist.c \
	    src/dircache.c src/rpc.c src/drc.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -o $@ tests/readdir2_test.c src/nfs2.c \
	    src/fs.c src/fh.c src/conf.c src/log.c src/nfs_common.c src/xdr.c \
	    src/dirlist.c src/dircache.c src/rpc.c src/drc.c

tests/probe_fh: tests/probe_fh.c
	$(CC) $(CFLAGS) -o $@ tests/probe_fh.c

check: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -f $(PROG) $(OBJS) $(TESTS) tests/probe_fh

.PHONY: all check clean
