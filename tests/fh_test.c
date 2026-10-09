/* File handle round-trip and per-request client ACL. Linux build only. */
#include "port.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "conf.h"
#include "fh.h"
#include "fs.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
	} while (0)

static struct in_addr
ip(const char *s)
{
	struct in_addr a;

	inet_pton(AF_INET, s, &a);
	return a;
}

int
main(void)
{
#ifdef __FreeBSD__
	printf("fh_test: skipped on FreeBSD\n");
	return 0;
#else
	char dir[] = "/tmp/fhtestXXXXXX";
	char exp[256];
	FILE *f;
	fhandle_t a, b, c;
	struct nfs_fh nfh;
	const struct export *ex;

	/* 1. Both halves of the fsid, dev and a 64-bit inode survive. */
	memset(&a, 0, sizeof(a));
	a.fh_fsid[0] = 0xdeadbeefu;
	a.fh_fsid[1] = 0x12345678u;
	a.fh_dev = 0x0102u;
	a.fh_ino = 0x1122334455667788ull;
	CHECK(fh_encode(&nfh, &a) == 0);
	CHECK(fh_valid(&nfh));
	CHECK(fh_decode(&nfh, &b) == 0);
	CHECK(b.fh_fsid[0] == a.fh_fsid[0]);
	CHECK(b.fh_fsid[1] == a.fh_fsid[1]);
	CHECK(b.fh_dev == a.fh_dev);
	CHECK(b.fh_ino == a.fh_ino);

	/* 2. Export lookup honors the client address on every call. */
	CHECK(mkdtemp(dir) != NULL);
	snprintf(exp, sizeof(exp), "%s/exports", dir);
	f = fopen(exp, "w");
	CHECK(f != NULL);
	fprintf(f, "%s -network 10.0.0.0 -mask 255.255.255.0\n", dir);
	fclose(f);
	CHECK(conf_load(exp) == 0);
	CHECK(conf_export_count() == 1);

	CHECK(port_lgetfh(dir, &c) == 0);
	CHECK(fh_encode(&nfh, &c) == 0);

	conf_set_client(ip("10.0.0.5"));
	ex = fh_lookup_export(&nfh);
	CHECK(ex != NULL);
	CHECK(!fh_export_ro(&nfh));

	conf_set_client(ip("192.168.1.9"));
	CHECK(fh_lookup_export(&nfh) == NULL);
	CHECK(fh_export_ro(&nfh));	/* denied client: never writable */

	/* 4. Permission check by mode bits, owner / group / other / root. */
	{
		struct fs_fattr at;
		uint32_t sup[2] = { 50, 60 };

		memset(&at, 0, sizeof(at));
		at.uid = 1000; at.gid = 100; at.mode = S_IFREG | 0640;
		CHECK(fs_access(&at, 1000, 1, 04) == 0);	/* owner read */
		CHECK(fs_access(&at, 1000, 1, 02) == 0);	/* owner write */
		CHECK(fs_access(&at, 1000, 1, 01) < 0);	/* no execute bit */
		CHECK(fs_access(&at, 2000, 100, 04) == 0);	/* group read */
		CHECK(fs_access(&at, 2000, 100, 02) < 0);	/* group no write */
		CHECK(fs_access(&at, 2000, 7, 04) < 0);	/* other: nothing */
		CHECK(fs_access_groups(&at, 2000, 7, sup, 2, 04) < 0);
		sup[1] = 100;
		CHECK(fs_access_groups(&at, 2000, 7, sup, 2, 04) == 0);
		CHECK(fs_access(&at, 0, 0, 06) == 0);		/* root rw */
		CHECK(fs_access(&at, 0, 0, 01) < 0);		/* root, no x bit */
		at.mode = S_IFDIR | 0700;
		CHECK(fs_access(&at, 0, 0, 01) == 0);		/* root, dir */
		CHECK(fs_access(&at, 1000, 1, 0100) == 0);	/* owner form 0100 */
		CHECK(fs_access(&at, 2000, 1, 0400) < 0);
	}

	/* 5. Credential mapping like exports(5). */
	{
		struct export e;
		uint32_t u, g;

		memset(&e, 0, sizeof(e));
		u = 0; g = 0;			/* default: root squash */
		conf_map_cred(&e, &u, &g);
		CHECK(u == CONF_NOBODY_UID && g == CONF_NOBODY_GID);
		u = 500; g = 500;		/* others unchanged */
		conf_map_cred(&e, &u, &g);
		CHECK(u == 500 && g == 500);
		e.maproot_set = 1; e.maproot_uid = 0; e.maproot_gid = 0;
		u = 0; g = 0;			/* -maproot=root: root stays root */
		conf_map_cred(&e, &u, &g);
		CHECK(u == 0);
		e.maproot_uid = 77; e.maproot_gid = 78;
		u = 0; g = 0;
		conf_map_cred(&e, &u, &g);
		CHECK(u == 77 && g == 78);
		u = 500; g = 500;		/* -maproot leaves other users */
		conf_map_cred(&e, &u, &g);
		CHECK(u == 500);
		e.mapall = 1;			/* -mapall: everyone is mapped */
		u = 500; g = 500;
		conf_map_cred(&e, &u, &g);
		CHECK(u == 77 && g == 78);
	}

	/* 3. A failed reload keeps the old exports (SIGHUP safety). */
	CHECK(conf_load("/nonexistent/exports") < 0);
	CHECK(conf_export_count() == 1);
	conf_set_client(ip("10.0.0.5"));
	CHECK(fh_lookup_export(&nfh) != NULL);

	/* 6. Export boundary and symlink handles. */
	{
		char sub[300], other[300], file[300], lnk[300], ef[300];
		fhandle_t hs, ho, hf, hl;
		struct nfs_fh ns, no, nf;
		struct stat lst;

		snprintf(sub, sizeof(sub), "%s/sub", dir);
		snprintf(other, sizeof(other), "%s/other", dir);
		snprintf(file, sizeof(file), "%s/sub/file", dir);
		snprintf(lnk, sizeof(lnk), "%s/sub/link", dir);
		snprintf(ef, sizeof(ef), "%s/exports2", dir);
		CHECK(mkdir(sub, 0755) == 0);
		CHECK(mkdir(other, 0755) == 0);
		close(open(file, O_CREAT | O_WRONLY, 0644));
		CHECK(symlink("/etc/hostname", lnk) == 0);
		f = fopen(ef, "w");
		fprintf(f, "%s\n", sub);
		fclose(f);
		CHECK(conf_load(ef) == 0);
		conf_set_client(ip("10.0.0.5"));

		CHECK(port_lgetfh(sub, &hs) == 0);
		CHECK(port_lgetfh(other, &ho) == 0);
		CHECK(port_lgetfh(file, &hf) == 0);
		fh_encode(&ns, &hs); fh_encode(&no, &ho); fh_encode(&nf, &hf);
		fh_path_cache_add(&ns, sub);
		fh_path_cache_add(&no, other);
		fh_path_cache_add(&nf, file);

		CHECK(fh_lookup_export(&ns) != NULL);
		CHECK(fh_lookup_export(&nf) != NULL);
		/* same filesystem, but outside the export */
		CHECK(fh_lookup_export(&no) == NULL);

		/* a handle for a symlink names the link, not its target */
		CHECK(port_lgetfh(lnk, &hl) == 0);
		CHECK(lstat(lnk, &lst) == 0);
		CHECK(hl.fh_ino == lst.st_ino);
	}

	snprintf(exp, sizeof(exp), "rm -rf %s", dir);
	(void)system(exp);

	if (fails == 0)
		printf("fh_test: OK\n");
	return fails != 0;
#endif
}
