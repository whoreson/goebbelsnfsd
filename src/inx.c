#include "port.h"
#include "inx.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Entry in the index, on-disk format.
 * Entries are sorted by dev (ascending), then ino (ascending).
 */
struct inx_hdr {
	uint64_t magic;
	uint64_t timestamp;
	uint32_t count;
	uint8_t  padding[4];
};

/*
 * Build phase: walk the export and collect (dev, ino, path) entries.
 */
struct inx_entry {
	uint64_t dev;
	uint64_t ino;
	char     path[1];	/* flexible array */
};

static struct inx_entry **inx_entries;
static unsigned inx_nentries;
static unsigned inx_capacity;

static void
inx_add_entry(const char *path, uint64_t dev, uint64_t ino)
{
	size_t len = strlen(path);
	struct inx_entry *e;

	if (inx_nentries >= inx_capacity) {
	inx_capacity = inx_capacity == 0 ? 4096 : inx_capacity * 2;
	inx_entries = realloc(inx_entries,
	    inx_capacity * sizeof(*inx_entries));
	}
	e = malloc(sizeof(*e) + len + 1);
	e->dev = dev;
	e->ino = ino;
	memcpy(e->path, path, len + 1);
	inx_entries[inx_nentries++] = e;
}

static int
inx_cmp_entries(const void *a, const void *b)
{
	const struct inx_entry *const *pa = a;
	const struct inx_entry *const *pb = b;
	if ((*pa)->dev < (*pb)->dev) return -1;
	if ((*pa)->dev > (*pb)->dev) return 1;
	if ((*pa)->ino < (*pb)->ino) return -1;
	if ((*pa)->ino > (*pb)->ino) return 1;
	return 0;
}

/* Build only */

static int
inx_walk(const char *path, uint64_t want_dev)
{
	DIR *dir;
	struct dirent *ent;
	char child[PATH_MAX];
	struct stat sb;

	dir = opendir(path);
	if (dir == NULL)
	 return 0;

	for (;;) {
	 errno = 0;
	 ent = readdir(dir);
	 if (ent == NULL)
	 break;
	 if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
	 continue;

	 if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >=
	     (int)sizeof(child)) {
	  closedir(dir);
	  return 0;
	 }

	 if (lstat(child, &sb) < 0)
	  continue;

	 inx_add_entry(child, (uint64_t)sb.st_dev, (uint64_t)sb.st_ino);

	 if ((uint64_t)sb.st_dev == want_dev && S_ISDIR(sb.st_mode))
	  inx_walk(child, want_dev);
	}
	closedir(dir);
	return 0;
}

int
inx_build(const char *exportpath, const char *outfile)
{
	struct stat sb;
	int fd, i;
	size_t total;
	struct inx_hdr hdr;

	if (stat(exportpath, &sb) < 0) {
	 log_msg(L_WARN, "inx_build: stat %s: %s", exportpath,
	     strerror(errno));
	 return -1;
	}

	inx_walk(exportpath, (uint64_t)sb.st_dev);

	/* Sort entries */
	qsort(inx_entries, inx_nentries, sizeof(struct inx_entry *),
	    inx_cmp_entries);

	/* Write to file */
	fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
	 log_msg(L_WARN, "inx_build: open %s: %s", outfile,
	     strerror(errno));
	 return -1;
	}

	hdr.magic = INX_MAGIC;
	hdr.timestamp = (uint64_t)sb.st_mtime;
	hdr.count = (uint32_t)inx_nentries;
	memset(hdr.padding, 0, sizeof(hdr.padding));
	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
	 close(fd);
	 return -1;
	}

	total = sizeof(hdr);
	for (i = 0; i < inx_nentries; i++) {
	 uint64_t dev = inx_entries[i]->dev;
	 uint64_t ino = inx_entries[i]->ino;
	 uint32_t pathlen = (uint32_t)strlen(inx_entries[i]->path);

	 if (write(fd, &dev, sizeof(dev)) != (ssize_t)sizeof(dev)) goto fail;
	 if (write(fd, &ino, sizeof(ino)) != (ssize_t)sizeof(ino)) goto fail;
	 if (write(fd, &pathlen, sizeof(pathlen)) != (ssize_t)sizeof(pathlen)) goto fail;
	 if (write(fd, inx_entries[i]->path, pathlen) != (ssize_t)pathlen) goto fail;
	 total += sizeof(dev) + sizeof(ino) + sizeof(pathlen) + pathlen;
	}

	close(fd);
	log_msg(L_INFO, "inx_build: wrote %u entries (%zu bytes) to %s",
	    inx_nentries, total, outfile);

	/* Free entries */
	for (i = 0; i < inx_nentries; i++)
	 free(inx_entries[i]);
	free(inx_entries);
	inx_entries = NULL;
	inx_nentries = 0;
	inx_capacity = 0;
	return 0;

fail:
	close(fd);
	return -1;
}

/*
 * Lookup: memory-map the index file and binary-search for (dev, ino).
 * Returns a pointer into the mmap'd region (valid until process exit).
 * Returns NULL if not found.
 */
static struct inx_hdr *inx_map;
static off_t inx_mapsz;

const char *
inx_lookup(const char *infile, uint64_t dev, uint64_t ino)
{
	int fd;
	struct stat sb;
	void *map;
	char *ent_start, *ent_end;
	uint32_t i;

	/* Check if we already have a valid mapping */
	if (inx_map != NULL) {
	 ent_start = (char *)inx_map + sizeof(struct inx_hdr);
	 ent_end = (char *)inx_map + inx_mapsz;
	 for (i = 0; i < inx_map->count; i++) {
	  uint64_t ed, ei;
	  uint32_t pl;

	  if (ent_start + sizeof(ed) + sizeof(ei) + sizeof(pl) > ent_end)
	   break;
	  memcpy(&ed, ent_start, sizeof(ed));
	  memcpy(&ei, ent_start + sizeof(ed), sizeof(ei));
	  memcpy(&pl, ent_start + sizeof(ed) + sizeof(ei), sizeof(pl));
	  if (ed > dev)
	   break;
	  if (ed == dev && ei == ino) {
	   /* Found! Return the path */
	   return ent_start + sizeof(ed) + sizeof(ei) + sizeof(pl);
	  }
	  ent_start += sizeof(ed) + sizeof(ei) + sizeof(pl) + pl;
	 }
	 return NULL;
	}

	/* Map the file */
	fd = open(infile, O_RDONLY);
	if (fd < 0)
	 return NULL;
	if (fstat(fd, &sb) < 0) {
	 close(fd);
	 return NULL;
	}
	if (sb.st_size < (off_t)sizeof(struct inx_hdr)) {
	 close(fd);
	 return NULL;
	}
	map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
	 return NULL;

	inx_map = (struct inx_hdr *)map;
	inx_mapsz = sb.st_size;

	if (inx_map->magic != INX_MAGIC) {
	 munmap(map, sb.st_size);
	 inx_map = NULL;
	 return NULL;
	}

	/* Search */
	ent_start = (char *)inx_map + sizeof(struct inx_hdr);
	ent_end = (char *)inx_map + inx_mapsz;
	for (i = 0; i < inx_map->count; i++) {
	 uint64_t ed, ei;
	 uint32_t pl;

	 if (ent_start + sizeof(ed) + sizeof(ei) + sizeof(pl) > ent_end)
	  break;
	 memcpy(&ed, ent_start, sizeof(ed));
	 memcpy(&ei, ent_start + sizeof(ed), sizeof(ei));
	 memcpy(&pl, ent_start + sizeof(ed) + sizeof(ei), sizeof(pl));
	 if (ed > dev)
	  break;
	 if (ed == dev && ei == ino) {
	  /* Found! Return the path */
	  return ent_start + sizeof(ed) + sizeof(ei) + sizeof(pl);
	 }
	 ent_start += sizeof(ed) + sizeof(ei) + sizeof(pl) + pl;
	}
	return NULL;
}
