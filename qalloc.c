#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "qalloc.h"

#include <pthread.h>
#include <stdio.h>		       /* for perror() */
#include <stdlib.h>		       /* for exit() */
#include <sys/types.h>		       /* for mmap() */
#include <sys/mman.h>		       /* for mmap() */
#include <sys/stat.h>		       /* for open() */
#include <fcntl.h>		       /* for open() */
#include <unistd.h>		       /* for fstat() */
#include <inttypes.h>		       /* for funky print statements */
#include <math.h>		       /* for ceil() and floor() */
#include <string.h>		       /* for memset() */
#include <assert.h>

#ifndef PTHREAD_MUTEX_SMALL_ENOUGH
#warning The pthread_mutex_t structure is either too big or hasn't been checked. If you're compiling by hand, you can probably ignore this warning, or define PTHREAD_MUTEX_SMALL_ENOUGH to make it go away.
#endif

/* The pads and bitmaps in these two datastructures are where they are (namely,
 * next to each other, after the next ptrs and mutexes) because it makes the
 * compiler naturally align things, which aids in speed (not to mention
 * bus-error-avoidance) without lots of extra #define work. The data structures
 * are all packed (or should be) so that they're exactly 2048 bytes big (the
 * size of a basic block). */

#define SMALLBLOCK_SLICE_SIZE 64
#define SMALLBLOCK_SLICE_COUNT (1920/SMALLBLOCK_SLICE_SIZE)
#define SMALLBLOCK_BITMAP_LEN ((int)ceil((SMALLBLOCK_SLICE_COUNT)/8.0))
typedef char smallslice_t[SMALLBLOCK_SLICE_SIZE];
typedef struct smallblock_s
{
    void *next;
    pthread_mutex_t lock __attribute__ ((packed));
    unsigned char bitmap[SMALLBLOCK_BITMAP_LEN] __attribute__ ((packed));
    char pad[128 - SMALLBLOCK_BITMAP_LEN - sizeof(void *) -
	     sizeof(pthread_mutex_t)] __attribute__ ((packed));
    smallslice_t slices[SMALLBLOCK_SLICE_COUNT] __attribute__ ((packed));
}
smallblock_t;

#define BIGBLOCK_ENTRY_COUNT (1920/(sizeof(void*)+sizeof(unsigned int)))
#define BIGBLOCK_BITMAP_LEN ((int)ceil((BIGBLOCK_ENTRY_COUNT)/8.0))
typedef struct bigblock_header_s
{
    void *next;
    pthread_mutex_t lock __attribute__ ((packed));
    unsigned char bitmap[BIGBLOCK_BITMAP_LEN] __attribute__ ((packed));
    char pad[128 - BIGBLOCK_BITMAP_LEN - sizeof(void *) -
	     sizeof(pthread_mutex_t)] __attribute__ ((packed));
    struct
    {
	void *entry __attribute__ ((packed));
	unsigned int block_count __attribute__ ((packed));
    } entries[BIGBLOCK_ENTRY_COUNT] __attribute__ ((packed));
} bigblock_header_t;

struct dynmapinfo_s
{
    char dynflag;
    void *map;
    struct dynmapinfo_s *next;
    size_t size;		/* filesize */
    size_t streamcount;
    pthread_mutex_t *stream_locks;
    smallblock_t **smallblocks;
    bigblock_header_t **bigblocks;
    unsigned char *bitmap;
    size_t bitmaplength;
    pthread_mutex_t *bitmap_lock;
    void *base;
};

struct mapinfo_s
{
    char dynflag;
    void *map;
    struct mapinfo_s *next;
    size_t size;		/* filesize */
    size_t streamcount;
    void ***streams;
    pthread_mutex_t *stream_locks;
};

static struct mapinfo_s *mmaps = NULL;
static struct dynmapinfo_s *dynmmaps = NULL;

#ifdef __linux__
# define fstat fstat64
# define lseek lseek64
typedef struct stat64 statstruct_t;

#elif defined(__APPLE__)
# define O_NOATIME 0
typedef struct stat statstruct_t;

#endif

static inline void *qalloc_getfile(const off_t filesize, void *addr,
				   const char *filename, void **set)
{				       /*{{{ */
    int fd, rcount, fstatret;
    statstruct_t st;
    void *ret;

    fd = open(filename, O_RDWR | O_CREAT | O_NOATIME | O_NONBLOCK,
	      S_IRUSR | S_IWUSR);
    if (fd == -1) {
	perror("open");
	abort();
    }
    fstatret = fstat(fd, &st);
    if (fstatret != 0) {
	perror("fstat");
	abort();
    }
    if (st.st_size == 0) {
	/* new file */
	off_t tailend;
	char touch = 0;

	tailend = lseek(fd, filesize - 1, SEEK_SET);
	if (tailend != filesize - 1) {
	    perror("seeking to end of file");
	    abort();
	}
	if (write(fd, &touch, 1) != 1) {
	    perror("setting file size");
	    abort();
	}
	tailend = lseek(fd, 0, SEEK_SET);
	if (tailend != 0) {
	    perror("seeking back to beginning of file");
	    abort();
	}
    } else if (st.st_size != filesize) {
	fprintf(stderr,
		"file is the wrong size! Wanted %" PRIuMAX " but got %"
		PRIuMAX "\n", (uintmax_t) filesize, (uintmax_t) st.st_size);
	abort();
    }
    rcount = read(fd, set, sizeof(void *));
    if (rcount != sizeof(void *)) {
	perror("reading base ptr");
	abort();
    }
    ret =
	mmap(addr, (size_t) filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	     0);
    if (ret == NULL || ret == (void *)-1) {
	/* could not mmap() */
	perror("mmap");
	abort();
    }
    return ret;
}				       /*}}} */

void *qalloc_makestatmap(const off_t filesize, void *addr,
			 const char *filename, size_t itemsize,
			 const size_t streams)
{				       /*{{{ */
    void *set, *ret;

    ret = qalloc_getfile(filesize, addr, filename, &set);
    if (set == NULL) {
	/* ptr is the address returned by mmap() */
	void **ptr = (void **)ret;

	/* base is the first address of usable (i.e. non-overhead) memory */
	char *base =
	    ((char *)(ptr + 3 + streams)) +
	    (sizeof(pthread_mutex_t) * streams);
	size_t i;
	void ***strms;
	struct mapinfo_s *mi;

	mi = malloc(sizeof(struct mapinfo_s));
	mi->dynflag = 0;
	/* never mmapped anything before */
	mi->map = ptr[0] = ret;
	/* 32-bit alignment */
	itemsize = (((itemsize - 1) / 4) + 1) * 4;
	ptr[1] = (void *)itemsize;
	ptr[2] = (void *)streams;
	strms = malloc(sizeof(void **) * streams);
	mi->size = (size_t) filesize;
	mi->streams = (void ***)(ptr + 3);
	mi->stream_locks = (pthread_mutex_t *) (ptr + 3 + streams);
	mi->streamcount = streams;
	mi->next = mmaps;
	mmaps = mi;
	/* initialize the streams */
	i = 0;
	for (i = 0; i < streams; ++i) {
	    strms[i] = ptr[3 + i] = (void *)(base + (itemsize * i));
	    assert(pthread_mutex_init(mi->stream_locks + i, NULL) == 0);
	}
	base += itemsize * streams;
	/* now fill in the free lists */
	while (base + (itemsize * streams) < ((char *)ret) + filesize) {
	    for (i = 0; i < streams; ++i) {
		strms[i] = *strms[i] = base + (itemsize * i);
	    }
	    base += itemsize * streams;
	}
	for (i = 0; i < streams; ++i) {
	    if (base + (itemsize * i) >= ((char *)ret) + filesize)
		break;
	    strms[i] = *strms[i] = base + (itemsize * i);
	}
	free(strms);
	/* and just for safety's sake, let's sync it to disk */
	if (msync(ret, (size_t) filesize, MS_INVALIDATE | MS_SYNC) != 0) {
	    perror("msync");
	    //abort();
	}
	return mi;
    } else if (set != ret) {
	/* asked for it somewhere that it didn't appear */
	fprintf(stderr, "offset is nonzero: %i\n",
		(int)((size_t) set - (size_t) ret));
	abort();
    } else {
	/* reloading an existing file in the correct place */
	struct mapinfo_s *m;
	m = malloc(sizeof(struct mapinfo_s));
	m->dynflag = 0;
	m->map = ret;
	m->size = (size_t) filesize;
	m->streams = (void ***)(((void **)ret) + 3);
	m->stream_locks = (pthread_mutex_t *) (((void **)ret) + 3 + streams);
	m->streamcount = streams;
	m->next = mmaps;
	mmaps = m;
	return m;
    }
}				       /*}}} */

void *qalloc_makedynmap(const off_t filesize, void *addr,
			const char *filename, const size_t streams)
{				       /*{{{ */
    void *set, *ret;

    ret = qalloc_getfile(filesize, addr, filename, &set);
    if (set == NULL) {
	/* never mmapped anything before */
	/* ptr is the address returned by mmap() */
	void **ptr = (void **)ret;
	size_t i;
	struct dynmapinfo_s *mi;

	mi = malloc(sizeof(struct dynmapinfo_s));
	mi->dynflag = 1;
	/* save the base address, so relocation can be detected (or corrected) */
	ptr[0] = mi->map = ret;
	ptr[1] = 0;		       /* dynamic */
	ptr[2] = (void *)streams;
	mi->streamcount = streams;
	mi->size = (size_t) filesize;
	mi->smallblocks = (smallblock_t **) (ptr + 3);
	mi->bigblocks = (bigblock_header_t **) (mi->smallblocks + streams);
	mi->stream_locks = (pthread_mutex_t *) (mi->bigblocks + streams);
	mi->bitmap_lock = (pthread_mutex_t *) (mi->stream_locks + streams);
	mi->bitmap = (unsigned char *)(mi->bitmap_lock + 1);
	mi->bitmaplength = (size_t) ceil((filesize / 2048) / 8.0);
	mi->base = ((char *)(mi->bitmap)) + mi->bitmaplength;
	/* initialize the streams */
	i = 0;
	for (i = 0; i < streams; ++i) {
	    mi->smallblocks[i] = NULL; /* the smallblock pointer */
	    mi->bigblocks[i] = NULL;   /* the bigblock pointer */
	    assert(pthread_mutex_init(mi->stream_locks + i, NULL) == 0);
	}
	/* initialize the use bitmap */
	memset(mi->bitmap, 0, mi->bitmaplength);
	assert(pthread_mutex_init(mi->bitmap_lock, NULL) == 0);
	return mi;
    } else if (set != ret) {
	/* asked for it somewhere that it didn't appear */
	fprintf(stderr, "offset is nonzero: %i\n",
		(int)((size_t) set - (size_t) ret));
	abort();
    } else {
	/* reloading an existing file in the correct place */
	struct dynmapinfo_s *m;
	m = malloc(sizeof(struct dynmapinfo_s));
	m->dynflag = 1;
	m->map = ret;
	m->size = (size_t) filesize;
	m->streamcount = streams;
	m->smallblocks = (smallblock_t **) (((void **)ret) + 3);
	m->bigblocks = (bigblock_header_t **) (m->smallblocks + streams);
	m->stream_locks = (pthread_mutex_t *) (m->bigblocks + streams);
	m->bitmap_lock = (pthread_mutex_t *) (m->stream_locks + streams);
	m->bitmap = (unsigned char *)(m->bitmap_lock + 1);
	m->bitmaplength = (size_t) (ceil(floor(filesize / 2048.0) / 8.0));
	m->base = ((char *)(m->bitmap)) + m->bitmaplength;

	m->next = dynmmaps;
	dynmmaps = m;
	return m;
    }
}				       /*}}} */

void *qalloc_loadmap(const char *filename)
{				       /*{{{ */
    int fd, fstatret;
    statstruct_t st;
    off_t filesize;
    void **header[3];

    fd = open(filename, O_RDONLY, S_IRUSR | S_IWUSR);
    if (fd == -1) {
	perror("open");
	abort();
    }
    fstatret = fstat(fd, &st);
    if (fstatret != 0) {
	perror("fstat");
	abort();
    }
    filesize = st.st_size;
    if (read(fd, header, sizeof(header)) != sizeof(header)) {
	perror("read");
	abort();
    }
    if (close(fd) != 0) {
	perror("close");
	abort();
    }
    if (header[1] != NULL) {
	return qalloc_makestatmap(filesize, header[0], filename,
				  (size_t) (header[1]), (size_t) (header[2]));
    } else {
	return qalloc_makedynmap(filesize, header[0], filename,
				 (size_t) (header[2]));
    }
}				       /*}}} */

void qalloc_cleanup()
{				       /*{{{ */
    qalloc_checkpoint();
    while (mmaps) {
	struct mapinfo_s *m;

	if (munmap(mmaps->map, mmaps->size) != 0) {
	    perror("munmap");
	    abort();
	}
	m = mmaps;
	mmaps = mmaps->next;
	free(m);
    }
    while (dynmmaps) {
	struct dynmapinfo_s *m;

	if (munmap(dynmmaps->map, dynmmaps->size) != 0) {
	    perror("munmap");
	    abort();
	}
	m = dynmmaps;
	dynmmaps = dynmmaps->next;
	free(m);
    }
}				       /*}}} */

/* this is inefficient in the case of running out of memory because of malloc
 * imbalance (i.e. one thread is making all of the qalloc_malloc() calls).
 * Could probably do more aggressive memory stealing from the next stream if
 * that becomes a problem */
void *qalloc_statmalloc(struct mapinfo_s *m)
{				       /*{{{ */
    pthread_t me = pthread_self();
    size_t stream = (size_t) me % m->streamcount;
    size_t firststream = stream;
    void **ret = NULL;

    while (ret == NULL) {
	assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
	ret = m->streams[stream];
	m->streams[stream] = *(m->streams[stream]);
	assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	if (ret == NULL) {
	    /* no more memory left in this stream */
	    stream = (stream + 1) % m->streamcount;
	    if (stream == firststream) {
		return NULL;
	    }
	}
    }
    return ret;
}				       /*}}} */

static inline void qalloc_unmarkbits(unsigned char *array, size_t startbit,
				     size_t count)
{				       /*{{{ */
    size_t endbit = startbit + count - 1;

    if (startbit / 8 == endbit / 8) {
	/* unmarking bits within a byte */
	unsigned char bitmask;

	bitmask = 0xff >> (startbit - ((startbit / 8) * 8));
	bitmask &= 0xff & (0xff80 >> (endbit - ((endbit / 8) * 8)));
	array[startbit / 8] &= ~bitmask;
    } else {
	size_t i;

	/* unmark the last part of startbyte */
	array[startbit / 8] &=
	    0xff & ~(0xff >> (startbit - ((startbit / 8) * 8)));
	/* unmark all bytes inbetween start and end bytes */
	for (i = startbit / 8 + 1; i <= endbit / 8 - 1; i++) {
	    array[i] = 0;
	}
	/* unmark the first part of the endbyte */
	array[endbit / 8] &=
	    0xff & ~(0xff80 >> (endbit - ((endbit / 8) * 8)));
    }
}				       /*}}} */

static inline void qalloc_markbits(unsigned char *array, size_t startbit,
				   size_t endbit)
{				       /*{{{ */
    if (startbit / 8 == endbit / 8) {
	/* marking bits within a byte */
	unsigned char bitmask;

	bitmask = 0xff >> (startbit - ((startbit / 8) * 8));
	bitmask &= 0xff & (0xff80 >> (endbit - ((endbit / 8) * 8)));
	array[startbit / 8] |= bitmask;
    } else {
	size_t i;

	/* mark the last part of startbyte */
	array[startbit / 8] |= 0xff >> (startbit - ((startbit / 8) * 8));
	/* mark all bytes inbetween start and end bytes */
	for (i = startbit / 8 + 1; i <= endbit / 8 - 1; i++) {
	    array[i] = 0xff;
	}
	/* mark the first part of the endbyte */
	array[endbit / 8] |= 0xff & (0xff80 >> (endbit - ((endbit / 8) * 8)));
    }
}				       /*}}} */

/* this function finds the first 0 bit in the array, toggles it, and returns
 * the index of the toggled bit. if no 0 bits are in the array, it returns
 * (a_len * 8 + 8) */
static inline size_t qalloc_findmark_bit(unsigned char *array, size_t bits)
{				       /*{{{ */
    size_t i;
    size_t bytes = bits / 8;

    for (i = 0; i < bytes && array[i] == 0xff; ++i) ;
    if (i == bytes) {
	return bytes * 8 + 8;
    } else if ((array[i] & 0x80) == 0) {
	array[i] |= 0x80;
	return i * 8;
    } else if ((array[i] & 0x40) == 0) {
	array[i] |= 0x40;
	return i * 8 + 1;
    } else if ((array[i] & 0x20) == 0) {
	array[i] |= 0x20;
	return i * 8 + 2;
    } else if ((array[i] & 0x10) == 0) {
	array[i] |= 0x10;
	return i * 8 + 3;
    } else if ((array[i] & 0x08) == 0) {
	array[i] |= 0x08;
	return i * 8 + 4;
    } else if ((array[i] & 0x04) == 0) {
	array[i] |= 0x04;
	return i * 8 + 5;
    } else if ((array[i] & 0x02) == 0) {
	array[i] |= 0x02;
	return i * 8 + 6;
    } else if ((array[i] & 0x01) == 0) {
	array[i] |= 0x01;
	return i * 8 + 7;
    }
    /* this should never happen */
    return (size_t) - 1;
}				       /*}}} */

/* this function is identical to qalloc_findmark_bit, except that it searches
 * for a string of count 0 bits, and returns the index of the first one found. */
static inline size_t qalloc_findmark_bits(unsigned char *array, size_t a_len,
					  size_t count)
{				       /*{{{ */
    size_t byte = 0;
    size_t startbit = -1;

    if (count < 8) {
	for (byte = 0; byte < a_len; ++byte) {
	    char mask = (0xff00 >> count) & 0xff;
	    char bit = 0;

	    while (!mask & 0x1) {
		char xor = mask ^ array[byte];

		if ((array[byte] & xor) == array[byte]) {
		    startbit = byte * 8 + bit;
		    qalloc_markbits(array, startbit, startbit + count - 1);
		    return startbit;
		}
		mask >>= 1;
		bit++;
	    }
	}
	return -1;
    } else {
	long int left_to_find;

      stageone:
	left_to_find = (long int)count;
	/* step 1: find bytes ending in 0's: */
	for (; byte < a_len; ++byte) {
	    char available = 0;

	    if (array[byte] & 0x1) {
		continue;
	    } else if (!array[byte] & 0xff) {
		available = 8;
	    } else if (!array[byte] & 0x7f) {
		available = 7;
	    } else if (!array[byte] & 0x3f) {
		available = 6;
	    } else if (!array[byte] & 0x1f) {
		available = 5;
	    } else if (!array[byte] & 0x0f) {
		available = 4;
	    } else if (!array[byte] & 0x07) {
		available = 3;
	    } else if (!array[byte] & 0x03) {
		available = 2;
	    } else if (!array[byte] & 0x01) {
		available = 1;
	    }
	    if (available != 0) {
		/* got a live one! */
		startbit = (byte * 8) + (8 - available);
		left_to_find -= available;
		if (left_to_find == 0) {
		    array[byte] = 0xff;
		    return startbit;
		} else if (left_to_find < 8) {
		    goto stagethree;
		} else {
		    goto stagetwo;
		}
	    }
	}
	return -1;
      stagetwo:
	byte++;
	/* step 2: make sure we have enough blank bytes next */
	for (; byte < a_len; ++byte) {
	    if (array[byte] == 0) {
		left_to_find -= 8;
		if (left_to_find < 8) {
		    goto stagethree;
		}
	    } else {
		goto stageone;
	    }
	}
	return -1;
      stagethree:
	byte++;
	/* step 3: make sure the last byte has enough blanks */
	if (array[byte] & (0xff & (0xff00 >> left_to_find))) {
	    /* not enough */
	    goto stageone;
	} else {
	    /* huzzah! we found enough space! */
	    qalloc_markbits(array, startbit, startbit + count - 1);
	    return startbit;
	}
    }
}				       /*}}} */

void *qalloc_dynmalloc(struct dynmapinfo_s *m, size_t size)
{				       /*{{{ */
    pthread_t me = pthread_self();
    size_t stream = (size_t) me % m->streamcount;
    void *ret = NULL;

    if (size <= 64) {
	size_t offset;
	smallblock_t *sb = NULL;

	assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
	sb = m->smallblocks[stream];
	if (sb)
	    assert(pthread_mutex_lock(&sb->lock) == 0);
	assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	/* chase down a smallblock slice */
	while (sb != NULL &&
	       ((offset =
		 qalloc_findmark_bit(sb->bitmap,
				     SMALLBLOCK_SLICE_COUNT)) >
		SMALLBLOCK_SLICE_COUNT)) {
	    smallblock_t *sb_prev = sb;

	    sb = sb->next;
	    if (sb) {
		assert(pthread_mutex_lock(&sb->lock) == 0);
	    }
	    assert(pthread_mutex_unlock(&sb_prev->lock) == 0);
	}
	if (sb == NULL) {
	    /* allocate a new smallblock */
	    assert(pthread_mutex_lock(m->bitmap_lock) == 0);
	    offset = qalloc_findmark_bit(m->bitmap, m->bitmaplength * 8);
	    assert(pthread_mutex_unlock(m->bitmap_lock) == 0);
	    if (offset > m->bitmaplength * 8) {
#warning should try looking for a slice in the other streams
		return NULL;
	    }
	    sb = ((smallblock_t *) (m->base)) + offset;
	    sb->bitmap[0] = 0;
	    sb->bitmap[1] = 0;
	    assert(pthread_mutex_init(&sb->lock, NULL) == 0);
	    assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
	    sb->next = m->smallblocks[stream];
	    m->smallblocks[stream] = sb;
	    assert(pthread_mutex_lock(&sb->lock) == 0);
	    assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	    /* we just created it, so we can do a shortcut: we know none of the
	     * slices are taken, we'll just take the first one */
	    sb->bitmap[0] = 0x80;
	    offset = 0;
	}
	ret = sb->slices + offset;
	assert(pthread_mutex_unlock(&sb->lock) == 0);
    } else {
	size_t offset, blocks = (size_t) ceil(size / 2048.0);
	bigblock_header_t *bbh = NULL;

	/* lock the bitmap */
	assert(pthread_mutex_lock(m->bitmap_lock + stream) == 0); {
	    /* find the necessary free block(s) and mark them in-use */
	    if (blocks > 1) {
		offset =
		    qalloc_findmark_bits(m->bitmap, m->bitmaplength, blocks);
	    } else {
		offset = qalloc_findmark_bit(m->bitmap, m->bitmaplength * 8);
	    }
	}
	assert(pthread_mutex_unlock(m->bitmap_lock + stream) == 0);
	if (offset > m->bitmaplength * 8) {
	    /* trying other streams won't help */
	    return NULL;
	}
	ret = ((bigblock_header_t *) (m->base)) + offset;
	assert(pthread_mutex_lock(m->stream_locks + stream) == 0); {
	    bbh = m->bigblocks[stream];
	    if (bbh)
		assert(pthread_mutex_lock(&bbh->lock) == 0);
	}
	assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	/* chase down a block entry */
	while (bbh != NULL &&
	       ((offset =
		 qalloc_findmark_bit(bbh->bitmap,
				     BIGBLOCK_ENTRY_COUNT)) >
		BIGBLOCK_ENTRY_COUNT)) {
	    bigblock_header_t *bbh_prev = bbh;

	    bbh = bbh->next;
	    if (bbh) {
		assert(pthread_mutex_lock(&bbh->lock) == 0);
	    }
	    assert(pthread_mutex_unlock(&bbh_prev->lock) == 0);
	}
	if (bbh == NULL) {
	    size_t newoffset;

	    /* allocate a new bigblock header */
	    assert(pthread_mutex_lock(m->bitmap_lock) == 0);
	    newoffset = qalloc_findmark_bit(m->bitmap, m->bitmaplength * 8);
	    assert(pthread_mutex_unlock(m->bitmap_lock) == 0);
	    if (newoffset > m->bitmaplength * 8) {
#warning should try looking for a bigblock header in other streams
		qalloc_unmarkbits(m->bitmap, offset, blocks);
		return NULL;
	    }
	    bbh = ((bigblock_header_t *) (m->base)) + newoffset;
	    memset(bbh->bitmap, 0, BIGBLOCK_BITMAP_LEN);
	    assert(pthread_mutex_init(&bbh->lock, NULL) == 0);
	    assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
	    bbh->next = m->bigblocks[stream];
	    m->bigblocks[stream] = bbh;
	    assert(pthread_mutex_lock(&bbh->lock) == 0);
	    assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	    offset = qalloc_findmark_bit(bbh->bitmap, BIGBLOCK_ENTRY_COUNT);
	}
	bbh->entries[offset].entry = ret;
	bbh->entries[offset].block_count = blocks;
	assert(pthread_mutex_unlock(&bbh->lock) == 0);
    }
    return ret;
}				       /*}}} */

void *qalloc_malloc(void *mapinfo, size_t size)
{
    if (((struct mapinfo_s *)mapinfo)->dynflag == 0) {
	return qalloc_statmalloc(mapinfo);
    } else {
	return qalloc_dynmalloc(mapinfo, size);
    }
}

void qalloc_statfree(void *block, struct mapinfo_s *m)
{				       /*{{{ */
    pthread_t me = pthread_self();
    size_t stream = (size_t) me % m->streamcount;
    void **b = (void **)block;

    assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
    *b = m->streams[stream];
    m->streams[stream] = b;
    assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
}				       /*}}} */

void qalloc_dynfree(void *block, struct dynmapinfo_s *m)
{				       /*{{{ */
    if (((size_t) block - (size_t) (m->base)) % 2048) {	/* unaligned */
	/* must be small */
	/* this figures out the sb pointer from the address being free'd */
	smallblock_t *sb =
	    (smallblock_t
	     *) ((((size_t) block - (size_t) (m->base)) & ~(size_t) 0x7ff) +
		 (size_t) (m->base));
	/* this figures out the slot number within the sb from the block address */
	unsigned int slot =
	    (((size_t) block) -
	     ((size_t) (sb->slices))) / SMALLBLOCK_SLICE_SIZE;
	unsigned char *byte;

	/* that slot (read: "bit") is in which byte? */
	byte = sb->bitmap + (slot / 8);
	/* which bit in that byte? */
	slot -= (slot / 8) * 8;
	/* QUICK! before anyone notices! */
	assert(pthread_mutex_lock(&sb->lock) == 0);
	*byte &= ~(0x80 >> slot);
	assert(pthread_mutex_unlock(&sb->lock) == 0);
    } else {			       /* aligned */
	/* must be big */
	pthread_t me = pthread_self();
	size_t stream = (size_t) me & m->streamcount;
	bigblock_header_t *bbh;
	size_t blocks;
	int stillLooking = 1;

	assert(pthread_mutex_lock(m->stream_locks + stream) == 0);
	bbh = m->bigblocks[stream];
	if (bbh)
	    assert(pthread_mutex_lock(&bbh->lock) == 0);
	assert(pthread_mutex_unlock(m->stream_locks + stream) == 0);
	/* chase down the bigblock header containing this ptr */
	while (bbh && stillLooking) {
	    size_t slot;
	    bigblock_header_t *next;

	    for (slot = 0; slot < BIGBLOCK_ENTRY_COUNT; ++slot) {
		if (bbh->entries[slot].entry == block) {
		    unsigned char *byte = bbh->bitmap + (slot / 8);
		    unsigned char bit = slot - ((slot / 8) * 8);

		    *byte &= ~(0x80 >> bit);
		    blocks = bbh->entries[slot].block_count;
		    bbh->entries[slot].entry = NULL;
		    bbh->entries[slot].block_count = 0;
		    stillLooking = 0;
		    break;
		}
	    }
	    if (stillLooking) {
		break;
	    }
	    next = bbh->next;
	    if (next) {
		assert(pthread_mutex_lock(&next->lock) == 0);
	    }
	    assert(pthread_mutex_unlock(&bbh->lock) == 0);
	    bbh = next;
	}
	if (blocks > 0 && !stillLooking) {
	    /* lock the bitmap and unmark the corresponding bits */
	    assert(pthread_mutex_lock(m->bitmap_lock) == 0);
	    qalloc_unmarkbits(m->bitmap,
			      ((size_t) block - (size_t) (m->base)) / 2048,
			      blocks);
	    assert(pthread_mutex_unlock(m->bitmap_lock) == 0);
	}
    }
    /* XXX: consider freeing unused smallblocks or bigblock header blocks */
}				       /*}}} */

void qalloc_free(void *block, void *mapinfo)
{
    if (((struct mapinfo_s *)mapinfo)->dynflag == 0) {
	qalloc_statfree(block, mapinfo);
    } else {
	qalloc_dynfree(block, mapinfo);
    }
}

void qalloc_checkpoint()
{				       /*{{{ */
    struct mapinfo_s *m = mmaps;
    struct dynmapinfo_s *dm = dynmmaps;

    while (m) {
	if (msync(m->map, m->size, MS_INVALIDATE | MS_SYNC) != 0) {
	    perror("checkpoint");
	    //abort();
	}
	m = m->next;
    }
    while (dm) {
	if (msync(dm->map, dm->size, MS_INVALIDATE | MS_SYNC) != 0) {
	    perror("checkpoint");
	    //abort();
	}
	dm = dm->next;
    }
}				       /*}}} */
