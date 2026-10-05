PROG=	unfsd

OBJS=	src/main.o src/log.o src/xdr.o src/rpc.o src/net.o \
	src/portmap.o src/nfs2.o src/nfs3.o src/mount.o \
	src/conf.o src/fh.o src/fs.o src/nfs_common.o
HDRS=	src/log.h src/xdr.h src/rpc.h src/types.h src/net.h \
	src/portmap.h src/progs.h src/conf.h src/fh.h src/fs.h \
	src/nfs_common.h

CC?=	cc
CFLAGS=	-std=gnu89 -O2 -g -Wall -Wextra -Wno-long-long \
	-Wstrict-prototypes -Wmissing-prototypes -Wshadow \
	-Wpointer-arith -Wcast-qual -Wwrite-strings \
	-DPROGNAME=\"$(PROG)\"

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJS): $(HDRS)

tests/xdr_test: tests/xdr_test.c src/xdr.c src/xdr.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/xdr_test.c src/xdr.c

tests/probe_fh: tests/probe_fh.c
	$(CC) $(CFLAGS) -o $@ tests/probe_fh.c

check: tests/xdr_test
	./tests/xdr_test

clean:
	rm -f $(PROG) src/*.o tests/xdr_test tests/probe_fh

.PHONY: all check clean
