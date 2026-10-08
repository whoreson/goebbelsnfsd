PROG=	unfsd

SRCS=	src/main.c src/log.c src/xdr.c src/rpc.c src/net.c \
	src/portmap.c src/nfs2.c src/nfs3.c src/mount.c \
	src/conf.c src/fh.c src/fs.c src/nfs_common.c
OBJS=	$(SRCS:.c=.o)
HDRS=	src/log.h src/xdr.h src/rpc.h src/types.h src/net.h \
	src/portmap.h src/progs.h src/conf.h src/fh.h src/fs.h \
	src/nfs_common.h src/port.h

CC?=	cc
CFLAGS=	-O2 -g -Wall -Wextra -Wno-long-long \
	-Wshadow -Wpointer-arith -Wcast-qual -Wwrite-strings

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

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

tests/xdr_test: tests/xdr_test.c src/xdr.c src/xdr.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/xdr_test.c src/xdr.c

tests/probe_fh: tests/probe_fh.c
	$(CC) $(CFLAGS) -o $@ tests/probe_fh.c

check: tests/xdr_test
	./tests/xdr_test

clean:
	rm -f $(PROG) $(OBJS) tests/xdr_test tests/probe_fh

.PHONY: all check clean
