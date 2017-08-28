/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright (c) 1998-2017 Zend Technologies Ltd. (http://www.zend.com) |
   +----------------------------------------------------------------------+
   | This source file is subject to version 2.00 of the Zend license,     |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.zend.com/license/2_00.txt.                                |
   | If you did not receive a copy of the Zend license and are unable to  |
   | obtain it through the world-wide-web, please send a note to          |
   | license@zend.com so we can mail you a copy immediately.              |
   +----------------------------------------------------------------------+
   | Authors: Andi Gutmans <andi@zend.com>                                |
   |          Zeev Suraski <zeev@zend.com>                                |
   |          Dmitry Stogov <dmitry@zend.com>                             |
   +----------------------------------------------------------------------+
*/

/* $Id$ */

/*
 * zend_alloc is designed to be a modern CPU cache friendly memory manager
 * for PHP. Most ideas are taken from jemalloc and tcmalloc implementations.
 *
 * All allocations are split into 3 categories:
 *
 * Huge  - the size is greater than CHUNK size (~2M by default), allocation is
 *         performed using mmap(). The result is aligned on 2M boundary.
 *
 * Large - a number of 4096K pages inside a CHUNK. Large blocks
 *         are always aligned on page boundary.
 *
 * Small - less than 3/4 of page size. Small sizes are rounded up to nearest
 *         greater predefined small size (there are 30 predefined sizes:
 *         8, 16, 24, 32, ... 3072). Small blocks are allocated from
 *         RUNs. Each RUN is allocated as a single or few following pages.
 *         Allocation inside RUNs implemented using linked list of free
 *         elements. The result is aligned to 8 bytes.
 *
 * zend_alloc allocates memory from OS by CHUNKs, these CHUNKs and huge memory
 * blocks are always aligned to CHUNK boundary. So it's very easy to determine
 * the CHUNK owning the certain pointer. Regular CHUNKs reserve a single
 * page at start for special purpose. It contains bitset of free pages,
 * few bitset for available runs of predefined small sizes, map of pages that
 * keeps information about usage of each page in this CHUNK, etc.
 *
 * zend_alloc provides familiar emalloc/efree/erealloc API, but in addition it
 * provides specialized and optimized routines to allocate blocks of predefined
 * sizes (e.g. emalloc_2(), emallc_4(), ..., emalloc_large(), etc)
 * The library uses C preprocessor tricks that substitute calls to emalloc()
 * with more specialized routines when the requested size is known.
 */

#include "zend.h"
#include "zend_alloc.h"
#include "zend_globals.h"
#include "zend_operators.h"
#include "zend_multiply.h"
#include "zend_bitset.h"

#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#if HAVE_LIMITS_H
#include <limits.h>
#endif
#include <fcntl.h>
#include <errno.h>

# ifdef HAVE_MREMAP
#  ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#  endif
#  ifndef __USE_GNU
#   define __USE_GNU
#  endif
# endif
# include <sys/mman.h>
# ifndef MAP_ANON
#  ifdef MAP_ANONYMOUS
#   define MAP_ANON MAP_ANONYMOUS
#  endif
# endif
# ifndef MREMAP_MAYMOVE
#  define MREMAP_MAYMOVE 0
# endif
# ifndef MAP_FAILED
#  define MAP_FAILED ((void*)-1)
# endif
# ifndef MAP_POPULATE
#  define MAP_POPULATE 0
# endif
#  if defined(_SC_PAGESIZE) || (_SC_PAGE_SIZE)
#    define REAL_PAGE_SIZE _real_page_size
static size_t _real_page_size = ZEND_MM_PAGE_SIZE;
#  endif

#ifndef REAL_PAGE_SIZE
# define REAL_PAGE_SIZE ZEND_MM_PAGE_SIZE
#endif


#ifndef ZEND_MM_CHECK
# define ZEND_MM_CHECK(condition, message)  do { \
		if (UNEXPECTED(!(condition))) { \
			zend_mm_panic(message); \
		} \
	} while (0)
#endif

typedef uint32_t   zend_mm_page_info; /* 4-byte integer */
typedef zend_ulong zend_mm_bitset;    /* 4-byte or 8-byte integer */

#define ZEND_MM_ALIGNED_OFFSET(size, alignment) \
	(((size_t)(size)) & ((alignment) - 1))
#define ZEND_MM_ALIGNED_BASE(size, alignment) \
	(((size_t)(size)) & ~((alignment) - 1))
#define ZEND_MM_SIZE_TO_NUM(size, alignment) \
	(((size_t)(size) + ((alignment) - 1)) / (alignment))

#define ZEND_MM_BITSET_LEN		(sizeof(zend_mm_bitset) * 8)       /* 32 or 64 */
#define ZEND_MM_PAGE_MAP_LEN	(ZEND_MM_PAGES / ZEND_MM_BITSET_LEN) /* 16 or 8 */

typedef zend_mm_bitset zend_mm_page_map[ZEND_MM_PAGE_MAP_LEN];     /* 64B */

#define ZEND_MM_IS_FRUN                  0x00000000
#define ZEND_MM_IS_LRUN                  0x40000000
#define ZEND_MM_IS_SRUN                  0x80000000

#define ZEND_MM_LRUN_PAGES_MASK          0x000003ff
#define ZEND_MM_LRUN_PAGES_OFFSET        0

#define ZEND_MM_SRUN_BIN_NUM_MASK        0x0000001f
#define ZEND_MM_SRUN_BIN_NUM_OFFSET      0

#define ZEND_MM_SRUN_FREE_COUNTER_MASK   0x01ff0000
#define ZEND_MM_SRUN_FREE_COUNTER_OFFSET 16

#define ZEND_MM_NRUN_OFFSET_MASK         0x01ff0000
#define ZEND_MM_NRUN_OFFSET_OFFSET       16

#define ZEND_MM_LRUN_PAGES(info)         (((info) & ZEND_MM_LRUN_PAGES_MASK) >> ZEND_MM_LRUN_PAGES_OFFSET)
#define ZEND_MM_SRUN_BIN_NUM(info)       (((info) & ZEND_MM_SRUN_BIN_NUM_MASK) >> ZEND_MM_SRUN_BIN_NUM_OFFSET)
#define ZEND_MM_SRUN_FREE_COUNTER(info)  (((info) & ZEND_MM_SRUN_FREE_COUNTER_MASK) >> ZEND_MM_SRUN_FREE_COUNTER_OFFSET)
#define ZEND_MM_NRUN_OFFSET(info)        (((info) & ZEND_MM_NRUN_OFFSET_MASK) >> ZEND_MM_NRUN_OFFSET_OFFSET)

#define ZEND_MM_FRUN()                   ZEND_MM_IS_FRUN
#define ZEND_MM_LRUN(count)              (ZEND_MM_IS_LRUN | ((count) << ZEND_MM_LRUN_PAGES_OFFSET))
#define ZEND_MM_SRUN(bin_num)            (ZEND_MM_IS_SRUN | ((bin_num) << ZEND_MM_SRUN_BIN_NUM_OFFSET))
#define ZEND_MM_SRUN_EX(bin_num, count)  (ZEND_MM_IS_SRUN | ((bin_num) << ZEND_MM_SRUN_BIN_NUM_OFFSET) | ((count) << ZEND_MM_SRUN_FREE_COUNTER_OFFSET))
#define ZEND_MM_NRUN(bin_num, offset)    (ZEND_MM_IS_SRUN | ZEND_MM_IS_LRUN | ((bin_num) << ZEND_MM_SRUN_BIN_NUM_OFFSET) | ((offset) << ZEND_MM_NRUN_OFFSET_OFFSET))

#define ZEND_MM_BINS 30

typedef struct  _zend_mm_page      zend_mm_page;
typedef struct  _zend_mm_bin       zend_mm_bin;
typedef struct  _zend_mm_free_slot zend_mm_free_slot;
typedef struct  _zend_mm_chunk     zend_mm_chunk;
typedef struct  _zend_mm_huge_list zend_mm_huge_list;


/*
 * Memory is retrived from OS by chunks of fixed size 2MB.
 * Inside chunk it's managed by pages of fixed size 4096B.
 * So each chunk consists from 512 pages.
 * The first page of each chunk is reseved for chunk header.
 * It contains service information about all pages.
 *
 * free_pages - current number of free pages in this chunk
 *
 * free_tail  - number of continuous free pages at the end of chunk
 *
 * free_map   - bitset (a bit for each page). The bit is set if the corresponding
 *              page is allocated. Allocator for "lage sizes" may easily find a
 *              free page (or a continuous number of pages) searching for zero
 *              bits.
 *
 * map        - contains service information for each page. (32-bits for each
 *              page).
 *    usage:
 *				(2 bits)
 * 				FRUN - free page,
 *              LRUN - first page of "large" allocation
 *              SRUN - first page of a bin used for "small" allocation
 *
 *    lrun_pages:
 *              (10 bits) number of allocated pages
 *
 *    srun_bin_num:
 *              (5 bits) bin number (e.g. 0 for sizes 0-2, 1 for 3-4,
 *               2 for 5-8, 3 for 9-16 etc) see zend_alloc_sizes.h
 */

struct _zend_mm_heap {
	size_t             size;                    /* current memory usage */
	size_t             peak;                    /* peak memory usage */
	zend_mm_free_slot *free_slot[ZEND_MM_BINS]; /* free lists for small sizes */
	size_t             real_size;               /* current size of allocated pages */
	size_t             real_peak;               /* peak size of allocated pages */
	size_t             limit;                   /* memory limit */
	int                overflow;                /* memory overflow flag */

	zend_mm_huge_list *huge_list;               /* list of huge allocated blocks */

	zend_mm_chunk     *main_chunk;
	zend_mm_chunk     *cached_chunks;			/* list of unused chunks */
	int                chunks_count;			/* number of alocated chunks */
	int                peak_chunks_count;		/* peak number of allocated chunks for current request */
	int                cached_chunks_count;		/* number of cached chunks */
	double             avg_chunks_count;		/* average number of chunks allocated per request */
};

struct _zend_mm_chunk {
	zend_mm_heap      *heap;
	zend_mm_chunk     *next;
	zend_mm_chunk     *prev;
	uint32_t           free_pages;				/* number of free pages */
	uint32_t           free_tail;               /* number of free pages at the end of chunk */
	uint32_t           num;
	char               reserve[64 - (sizeof(void*) * 3 + sizeof(int) * 3)];
	zend_mm_heap       heap_slot;               /* used only in main chunk */
	zend_mm_page_map   free_map;                /* 512 bits or 64 bytes */
	zend_mm_page_info  map[ZEND_MM_PAGES];      /* 2 KB = 512 * 4 */
};

struct _zend_mm_page {
	char               bytes[ZEND_MM_PAGE_SIZE];
};

/*
 * bin - is one or few continuous pages (up to 8) used for allocation of
 * a particular "small size".
 */
struct _zend_mm_bin {
	char               bytes[ZEND_MM_PAGE_SIZE * 8];
};

struct _zend_mm_free_slot {
	zend_mm_free_slot *next_free_slot;
};

struct _zend_mm_huge_list {
	void              *ptr;
	size_t             size;
	zend_mm_huge_list *next;
};

#define ZEND_MM_PAGE_ADDR(chunk, page_num) \
	((void*)(((zend_mm_page*)(chunk)) + (page_num)))

#define _BIN_DATA_SIZE(num, size, elements, pages, x, y) size,
static const uint32_t bin_data_size[] = {
  ZEND_MM_BINS_INFO(_BIN_DATA_SIZE, x, y)
};

#define _BIN_DATA_ELEMENTS(num, size, elements, pages, x, y) elements,
static const uint32_t bin_elements[] = {
  ZEND_MM_BINS_INFO(_BIN_DATA_ELEMENTS, x, y)
};

#define _BIN_DATA_PAGES(num, size, elements, pages, x, y) pages,
static const uint32_t bin_pages[] = {
  ZEND_MM_BINS_INFO(_BIN_DATA_PAGES, x, y)
};


static ZEND_COLD ZEND_NORETURN void zend_mm_panic(const char *message)
{
	fprintf(stderr, "%s\n", message);
/* See http://support.microsoft.com/kb/190351 */
	exit(1);
}

static ZEND_COLD ZEND_NORETURN void zend_mm_safe_error(zend_mm_heap *heap,
	const char *format,
	size_t limit,
	size_t size)
{

	heap->overflow = 1;
	zend_try {
		zend_error_noreturn(E_ERROR,
			format,
			limit,
			size);
	} zend_catch {
	}  zend_end_try();
	heap->overflow = 0;
	zend_bailout();
	exit(1);
}


/*****************/
/* OS Allocation */
/*****************/

static void *zend_mm_mmap_fixed(void *addr, size_t size)
{
	/* MAP_FIXED leads to discarding of the old mapping, so it can't be used. */
	void *ptr = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON /*| MAP_POPULATE | MAP_HUGETLB*/, -1, 0);

	if (ptr == MAP_FAILED) {
		fprintf(stderr, "\nmmap() failed: [%d] %s\n", errno, strerror(errno));
		return NULL;
	} else if (ptr != addr) {
		if (munmap(ptr, size) != 0) {
			fprintf(stderr, "\nmunmap() failed: [%d] %s\n", errno, strerror(errno));
		}
		return NULL;
	}
	return ptr;
}

static void *zend_mm_mmap(size_t size)
{
	void *ptr;


	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

	if (ptr == MAP_FAILED) {
		fprintf(stderr, "\nmmap() failed: [%d] %s\n", errno, strerror(errno));
		return NULL;
	}
	return ptr;
}

static void zend_mm_munmap(void *addr, size_t size)
{
	if (munmap(addr, size) != 0) {
		fprintf(stderr, "\nmunmap() failed: [%d] %s\n", errno, strerror(errno));
	}
}

/***********/
/* Bitmask */
/***********/

/* number of trailing set (1) bits */
static zend_always_inline int zend_mm_bitset_nts(zend_mm_bitset bitset)
{
#if (defined(__GNUC__) || __has_builtin(__builtin_ctzl)) && SIZEOF_ZEND_LONG == SIZEOF_LONG && defined(PHP_HAVE_BUILTIN_CTZL)
	return __builtin_ctzl(~bitset);
#elif (defined(__GNUC__) || __has_builtin(__builtin_ctzll)) && defined(PHP_HAVE_BUILTIN_CTZLL)
	return __builtin_ctzll(~bitset);
#else
	int n;

	if (bitset == (zend_mm_bitset)-1) return ZEND_MM_BITSET_LEN;

	n = 0;
#if SIZEOF_ZEND_LONG == 8
	if (sizeof(zend_mm_bitset) == 8) {
		if ((bitset & 0xffffffff) == 0xffffffff) {n += 32; bitset = bitset >> Z_UL(32);}
	}
#endif
	if ((bitset & 0x0000ffff) == 0x0000ffff) {n += 16; bitset = bitset >> 16;}
	if ((bitset & 0x000000ff) == 0x000000ff) {n +=  8; bitset = bitset >>  8;}
	if ((bitset & 0x0000000f) == 0x0000000f) {n +=  4; bitset = bitset >>  4;}
	if ((bitset & 0x00000003) == 0x00000003) {n +=  2; bitset = bitset >>  2;}
	return n + (bitset & 1);
#endif
}

static zend_always_inline int zend_mm_bitset_find_zero(zend_mm_bitset *bitset, int size)
{
	int i = 0;

	do {
		zend_mm_bitset tmp = bitset[i];
		if (tmp != (zend_mm_bitset)-1) {
			return i * ZEND_MM_BITSET_LEN + zend_mm_bitset_nts(tmp);
		}
		i++;
	} while (i < size);
	return -1;
}

static zend_always_inline int zend_mm_bitset_find_one(zend_mm_bitset *bitset, int size)
{
	int i = 0;

	do {
		zend_mm_bitset tmp = bitset[i];
		if (tmp != 0) {
			return i * ZEND_MM_BITSET_LEN + zend_ulong_ntz(tmp);
		}
		i++;
	} while (i < size);
	return -1;
}

static zend_always_inline int zend_mm_bitset_find_zero_and_set(zend_mm_bitset *bitset, int size)
{
	int i = 0;

	do {
		zend_mm_bitset tmp = bitset[i];
		if (tmp != (zend_mm_bitset)-1) {
			int n = zend_mm_bitset_nts(tmp);
			bitset[i] |= Z_UL(1) << n;
			return i * ZEND_MM_BITSET_LEN + n;
		}
		i++;
	} while (i < size);
	return -1;
}

static zend_always_inline int zend_mm_bitset_is_set(zend_mm_bitset *bitset, int bit)
{
	return (bitset[bit / ZEND_MM_BITSET_LEN] & (Z_L(1) << (bit & (ZEND_MM_BITSET_LEN-1)))) != 0;
}

static zend_always_inline void zend_mm_bitset_set_bit(zend_mm_bitset *bitset, int bit)
{
	bitset[bit / ZEND_MM_BITSET_LEN] |= (Z_L(1) << (bit & (ZEND_MM_BITSET_LEN-1)));
}

static zend_always_inline void zend_mm_bitset_reset_bit(zend_mm_bitset *bitset, int bit)
{
	bitset[bit / ZEND_MM_BITSET_LEN] &= ~(Z_L(1) << (bit & (ZEND_MM_BITSET_LEN-1)));
}

static zend_always_inline void zend_mm_bitset_set_range(zend_mm_bitset *bitset, int start, int len)
{
	if (len == 1) {
		zend_mm_bitset_set_bit(bitset, start);
	} else {
		int pos = start / ZEND_MM_BITSET_LEN;
		int end = (start + len - 1) / ZEND_MM_BITSET_LEN;
		int bit = start & (ZEND_MM_BITSET_LEN - 1);
		zend_mm_bitset tmp;

		if (pos != end) {
			/* set bits from "bit" to ZEND_MM_BITSET_LEN-1 */
			tmp = (zend_mm_bitset)-1 << bit;
			bitset[pos++] |= tmp;
			while (pos != end) {
				/* set all bits */
				bitset[pos++] = (zend_mm_bitset)-1;
			}
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* set bits from "0" to "end" */
			tmp = (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			bitset[pos] |= tmp;
		} else {
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* set bits from "bit" to "end" */
			tmp = (zend_mm_bitset)-1 << bit;
			tmp &= (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			bitset[pos] |= tmp;
		}
	}
}

static zend_always_inline void zend_mm_bitset_reset_range(zend_mm_bitset *bitset, int start, int len)
{
	if (len == 1) {
		zend_mm_bitset_reset_bit(bitset, start);
	} else {
		int pos = start / ZEND_MM_BITSET_LEN;
		int end = (start + len - 1) / ZEND_MM_BITSET_LEN;
		int bit = start & (ZEND_MM_BITSET_LEN - 1);
		zend_mm_bitset tmp;

		if (pos != end) {
			/* reset bits from "bit" to ZEND_MM_BITSET_LEN-1 */
			tmp = ~((Z_L(1) << bit) - 1);
			bitset[pos++] &= ~tmp;
			while (pos != end) {
				/* set all bits */
				bitset[pos++] = 0;
			}
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* reset bits from "0" to "end" */
			tmp = (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			bitset[pos] &= ~tmp;
		} else {
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* reset bits from "bit" to "end" */
			tmp = (zend_mm_bitset)-1 << bit;
			tmp &= (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			bitset[pos] &= ~tmp;
		}
	}
}

static zend_always_inline int zend_mm_bitset_is_free_range(zend_mm_bitset *bitset, int start, int len)
{
	if (len == 1) {
		return !zend_mm_bitset_is_set(bitset, start);
	} else {
		int pos = start / ZEND_MM_BITSET_LEN;
		int end = (start + len - 1) / ZEND_MM_BITSET_LEN;
		int bit = start & (ZEND_MM_BITSET_LEN - 1);
		zend_mm_bitset tmp;

		if (pos != end) {
			/* set bits from "bit" to ZEND_MM_BITSET_LEN-1 */
			tmp = (zend_mm_bitset)-1 << bit;
			if ((bitset[pos++] & tmp) != 0) {
				return 0;
			}
			while (pos != end) {
				/* set all bits */
				if (bitset[pos++] != 0) {
					return 0;
				}
			}
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* set bits from "0" to "end" */
			tmp = (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			return (bitset[pos] & tmp) == 0;
		} else {
			end = (start + len - 1) & (ZEND_MM_BITSET_LEN - 1);
			/* set bits from "bit" to "end" */
			tmp = (zend_mm_bitset)-1 << bit;
			tmp &= (zend_mm_bitset)-1 >> ((ZEND_MM_BITSET_LEN - 1) - end);
			return (bitset[pos] & tmp) == 0;
		}
	}
}

/**********/
/* Chunks */
/**********/

static void *zend_mm_chunk_alloc_int(size_t size, size_t alignment)
{
	void *ptr = zend_mm_mmap(size);

	if (ptr == NULL) {
		return NULL;
	} else if (ZEND_MM_ALIGNED_OFFSET(ptr, alignment) == 0) {
#ifdef MADV_HUGEPAGE
	    madvise(ptr, size, MADV_HUGEPAGE);
#endif
		return ptr;
	} else {
		size_t offset;

		/* chunk has to be aligned */
		zend_mm_munmap(ptr, size);
		ptr = zend_mm_mmap(size + alignment - REAL_PAGE_SIZE);
		offset = ZEND_MM_ALIGNED_OFFSET(ptr, alignment);
		if (offset != 0) {
			offset = alignment - offset;
			zend_mm_munmap(ptr, offset);
			ptr = (char*)ptr + offset;
			alignment -= offset;
		}
		if (alignment > REAL_PAGE_SIZE) {
			zend_mm_munmap((char*)ptr + size, alignment - REAL_PAGE_SIZE);
		}
# ifdef MADV_HUGEPAGE
	    madvise(ptr, size, MADV_HUGEPAGE);
# endif
		return ptr;
	}
}

static void *zend_mm_chunk_alloc(zend_mm_heap *heap, size_t size, size_t alignment)
{
	return zend_mm_chunk_alloc_int(size, alignment);
}

static void zend_mm_chunk_free(zend_mm_heap *heap, void *addr, size_t size)
{
	zend_mm_munmap(addr, size);
}

static int zend_mm_chunk_truncate(zend_mm_heap *heap, void *addr, size_t old_size, size_t new_size)
{
	zend_mm_munmap((char*)addr + new_size, old_size - new_size);
	return 1;
}

static int zend_mm_chunk_extend(zend_mm_heap *heap, void *addr, size_t old_size, size_t new_size)
{
	return (zend_mm_mmap_fixed((char*)addr + old_size, new_size - old_size) != NULL);
}

static zend_always_inline void zend_mm_chunk_init(zend_mm_heap *heap, zend_mm_chunk *chunk)
{
	chunk->heap = heap;
	chunk->next = heap->main_chunk;
	chunk->prev = heap->main_chunk->prev;
	chunk->prev->next = chunk;
	chunk->next->prev = chunk;
	/* mark first pages as allocated */
	chunk->free_pages = ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE;
	chunk->free_tail = ZEND_MM_FIRST_PAGE;
	/* the younger chunks have bigger number */
	chunk->num = chunk->prev->num + 1;
	/* mark first pages as allocated */
	chunk->free_map[0] = (1L << ZEND_MM_FIRST_PAGE) - 1;
	chunk->map[0] = ZEND_MM_LRUN(ZEND_MM_FIRST_PAGE);
}

/***********************/
/* Huge Runs (forward) */
/***********************/

static size_t zend_mm_get_huge_block_size(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC);
static void *zend_mm_alloc_huge(zend_mm_heap *heap, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC);
static void zend_mm_free_huge(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC);

static void zend_mm_change_huge_block_size(zend_mm_heap *heap, void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC);

/**************/
/* Large Runs */
/**************/

static void *zend_mm_alloc_pages(zend_mm_heap *heap, uint32_t pages_count ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_chunk *chunk = heap->main_chunk;
	uint32_t page_num, len;
	int steps = 0;

	while (1) {
		if (UNEXPECTED(chunk->free_pages < pages_count)) {
			goto not_found;
		} else {
			/* Best-Fit Search */
			int best = -1;
			uint32_t best_len = ZEND_MM_PAGES;
			uint32_t free_tail = chunk->free_tail;
			zend_mm_bitset *bitset = chunk->free_map;
			zend_mm_bitset tmp = *(bitset++);
			uint32_t i = 0;

			while (1) {
				/* skip allocated blocks */
				while (tmp == (zend_mm_bitset)-1) {
					i += ZEND_MM_BITSET_LEN;
					if (i == ZEND_MM_PAGES) {
						if (best > 0) {
							page_num = best;
							goto found;
						} else {
							goto not_found;
						}
					}
					tmp = *(bitset++);
				}
				/* find first 0 bit */
				page_num = i + zend_mm_bitset_nts(tmp);
				/* reset bits from 0 to "bit" */
				tmp &= tmp + 1;
				/* skip free blocks */
				while (tmp == 0) {
					i += ZEND_MM_BITSET_LEN;
					if (i >= free_tail || i == ZEND_MM_PAGES) {
						len = ZEND_MM_PAGES - page_num;
						if (len >= pages_count && len < best_len) {
							chunk->free_tail = page_num + pages_count;
							goto found;
						} else {
							/* set accurate value */
							chunk->free_tail = page_num;
							if (best > 0) {
								page_num = best;
								goto found;
							} else {
								goto not_found;
							}
						}
					}
					tmp = *(bitset++);
				}
				/* find first 1 bit */
				len = i + zend_ulong_ntz(tmp) - page_num;
				if (len >= pages_count) {
					if (len == pages_count) {
						goto found;
					} else if (len < best_len) {
						best_len = len;
						best = page_num;
					}
				}
				/* set bits from 0 to "bit" */
				tmp |= tmp - 1;
			}
		}

not_found:
		if (chunk->next == heap->main_chunk) {
get_chunk:
			if (heap->cached_chunks) {
				heap->cached_chunks_count--;
				chunk = heap->cached_chunks;
				heap->cached_chunks = chunk->next;
			} else {
				if (UNEXPECTED(heap->real_size + ZEND_MM_CHUNK_SIZE > heap->limit)) {
					if (zend_mm_gc(heap)) {
						goto get_chunk;
					} else if (heap->overflow == 0) {
						zend_mm_safe_error(heap, "Allowed memory size of %zu bytes exhausted (tried to allocate %zu bytes)", heap->limit, ZEND_MM_PAGE_SIZE * pages_count);
						return NULL;
					}
				}
				chunk = (zend_mm_chunk*)zend_mm_chunk_alloc(heap, ZEND_MM_CHUNK_SIZE, ZEND_MM_CHUNK_SIZE);
				if (UNEXPECTED(chunk == NULL)) {
					/* insufficient memory */
					if (zend_mm_gc(heap) &&
					    (chunk = (zend_mm_chunk*)zend_mm_chunk_alloc(heap, ZEND_MM_CHUNK_SIZE, ZEND_MM_CHUNK_SIZE)) != NULL) {
						/* pass */
					} else {
						zend_mm_safe_error(heap, "Out of memory (allocated %zu) (tried to allocate %zu bytes)", heap->real_size, ZEND_MM_PAGE_SIZE * pages_count);
						return NULL;
					}
				}
				do {
					size_t size = heap->real_size + ZEND_MM_CHUNK_SIZE;
					size_t peak = MAX(heap->real_peak, size);
					heap->real_size = size;
					heap->real_peak = peak;
				} while (0);
			}
			heap->chunks_count++;
			if (heap->chunks_count > heap->peak_chunks_count) {
				heap->peak_chunks_count = heap->chunks_count;
			}
			zend_mm_chunk_init(heap, chunk);
			page_num = ZEND_MM_FIRST_PAGE;
			len = ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE;
			goto found;
		} else {
			chunk = chunk->next;
			steps++;
		}
	}

found:
	if (steps > 2 && pages_count < 8) {
		/* move chunk into the head of the linked-list */
		chunk->prev->next = chunk->next;
		chunk->next->prev = chunk->prev;
		chunk->next = heap->main_chunk->next;
		chunk->prev = heap->main_chunk;
		chunk->prev->next = chunk;
		chunk->next->prev = chunk;
	}
	/* mark run as allocated */
	chunk->free_pages -= pages_count;
	zend_mm_bitset_set_range(chunk->free_map, page_num, pages_count);
	chunk->map[page_num] = ZEND_MM_LRUN(pages_count);
	if (page_num == chunk->free_tail) {
		chunk->free_tail = page_num + pages_count;
	}
	return ZEND_MM_PAGE_ADDR(chunk, page_num);
}

static zend_always_inline void *zend_mm_alloc_large(zend_mm_heap *heap, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	int pages_count = (int)ZEND_MM_SIZE_TO_NUM(size, ZEND_MM_PAGE_SIZE);
	void *ptr = zend_mm_alloc_pages(heap, pages_count ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	do {
		size_t size = heap->size + pages_count * ZEND_MM_PAGE_SIZE;
		size_t peak = MAX(heap->peak, size);
		heap->size = size;
		heap->peak = peak;
	} while (0);
	return ptr;
}

static zend_always_inline void zend_mm_delete_chunk(zend_mm_heap *heap, zend_mm_chunk *chunk)
{
	chunk->next->prev = chunk->prev;
	chunk->prev->next = chunk->next;
	heap->chunks_count--;
	if (heap->chunks_count + heap->cached_chunks_count < heap->avg_chunks_count + 0.1) {
		/* delay deletion */
		heap->cached_chunks_count++;
		chunk->next = heap->cached_chunks;
		heap->cached_chunks = chunk;
	} else {
		heap->real_size -= ZEND_MM_CHUNK_SIZE;
		if (!heap->cached_chunks || chunk->num > heap->cached_chunks->num) {
			zend_mm_chunk_free(heap, chunk, ZEND_MM_CHUNK_SIZE);
		} else {
//TODO: select the best chunk to delete???
			chunk->next = heap->cached_chunks->next;
			zend_mm_chunk_free(heap, heap->cached_chunks, ZEND_MM_CHUNK_SIZE);
			heap->cached_chunks = chunk;
		}
	}
}

static zend_always_inline void zend_mm_free_pages_ex(zend_mm_heap *heap, zend_mm_chunk *chunk, uint32_t page_num, uint32_t pages_count, int free_chunk)
{
	chunk->free_pages += pages_count;
	zend_mm_bitset_reset_range(chunk->free_map, page_num, pages_count);
	chunk->map[page_num] = 0;
	if (chunk->free_tail == page_num + pages_count) {
		/* this setting may be not accurate */
		chunk->free_tail = page_num;
	}
	if (free_chunk && chunk->free_pages == ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE) {
		zend_mm_delete_chunk(heap, chunk);
	}
}

static void zend_mm_free_pages(zend_mm_heap *heap, zend_mm_chunk *chunk, int page_num, int pages_count)
{
	zend_mm_free_pages_ex(heap, chunk, page_num, pages_count, 1);
}

static zend_always_inline void zend_mm_free_large(zend_mm_heap *heap, zend_mm_chunk *chunk, int page_num, int pages_count)
{
	heap->size -= pages_count * ZEND_MM_PAGE_SIZE;
	zend_mm_free_pages(heap, chunk, page_num, pages_count);
}

/**************/
/* Small Runs */
/**************/

/* higher set bit number (0->N/A, 1->1, 2->2, 4->3, 8->4, 127->7, 128->8 etc) */
static zend_always_inline int zend_mm_small_size_to_bit(int size)
{
#if (defined(__GNUC__) || __has_builtin(__builtin_clz))  && defined(PHP_HAVE_BUILTIN_CLZ)
	return (__builtin_clz(size) ^ 0x1f) + 1;
#else
	int n = 16;
	if (size <= 0x00ff) {n -= 8; size = size << 8;}
	if (size <= 0x0fff) {n -= 4; size = size << 4;}
	if (size <= 0x3fff) {n -= 2; size = size << 2;}
	if (size <= 0x7fff) {n -= 1;}
	return n;
#endif
}

#ifndef MAX
# define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
# define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static zend_always_inline int zend_mm_small_size_to_bin(size_t size)
{
#if 0
	int n;
                            /*0,  1,  2,  3,  4,  5,  6,  7,  8,  9  10, 11, 12*/
	static const int f1[] = { 3,  3,  3,  3,  3,  3,  3,  4,  5,  6,  7,  8,  9};
	static const int f2[] = { 0,  0,  0,  0,  0,  0,  0,  4,  8, 12, 16, 20, 24};

	if (UNEXPECTED(size <= 2)) return 0;
	n = zend_mm_small_size_to_bit(size - 1);
	return ((size-1) >> f1[n]) + f2[n];
#else
	unsigned int t1, t2;

	if (size <= 64) {
		/* we need to support size == 0 ... */
		return (size - !!size) >> 3;
	} else {
		t1 = size - 1;
		t2 = zend_mm_small_size_to_bit(t1) - 3;
		t1 = t1 >> t2;
		t2 = t2 - 3;
		t2 = t2 << 2;
		return (int)(t1 + t2);
	}
#endif
}

#define ZEND_MM_SMALL_SIZE_TO_BIN(size)  zend_mm_small_size_to_bin(size)

static zend_never_inline void *zend_mm_alloc_small_slow(zend_mm_heap *heap, uint32_t bin_num ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
    zend_mm_chunk *chunk;
    int page_num;
	zend_mm_bin *bin;
	zend_mm_free_slot *p, *end;

	bin = (zend_mm_bin*)zend_mm_alloc_pages(heap, bin_pages[bin_num] ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	if (UNEXPECTED(bin == NULL)) {
		/* insufficient memory */
		return NULL;
	}

	chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(bin, ZEND_MM_CHUNK_SIZE);
	page_num = ZEND_MM_ALIGNED_OFFSET(bin, ZEND_MM_CHUNK_SIZE) / ZEND_MM_PAGE_SIZE;
	chunk->map[page_num] = ZEND_MM_SRUN(bin_num);
	if (bin_pages[bin_num] > 1) {
		uint32_t i = 1;

		do {
			chunk->map[page_num+i] = ZEND_MM_NRUN(bin_num, i);
			i++;
		} while (i < bin_pages[bin_num]);
	}

	/* create a linked list of elements from 1 to last */
	end = (zend_mm_free_slot*)((char*)bin + (bin_data_size[bin_num] * (bin_elements[bin_num] - 1)));
	heap->free_slot[bin_num] = p = (zend_mm_free_slot*)((char*)bin + bin_data_size[bin_num]);
	do {
		p->next_free_slot = (zend_mm_free_slot*)((char*)p + bin_data_size[bin_num]);;
		p = (zend_mm_free_slot*)((char*)p + bin_data_size[bin_num]);
	} while (p != end);

	/* terminate list using NULL */
	p->next_free_slot = NULL;

	/* return first element */
	return (char*)bin;
}

static zend_always_inline void *zend_mm_alloc_small(zend_mm_heap *heap, size_t size, int bin_num ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	do {
		size_t size = heap->size + bin_data_size[bin_num];
		size_t peak = MAX(heap->peak, size);
		heap->size = size;
		heap->peak = peak;
	} while (0);

	if (EXPECTED(heap->free_slot[bin_num] != NULL)) {
		zend_mm_free_slot *p = heap->free_slot[bin_num];
		heap->free_slot[bin_num] = p->next_free_slot;
		return (void*)p;
	} else {
		return zend_mm_alloc_small_slow(heap, bin_num ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	}
}

static zend_always_inline void zend_mm_free_small(zend_mm_heap *heap, void *ptr, int bin_num)
{
	zend_mm_free_slot *p;

	heap->size -= bin_data_size[bin_num];


    p = (zend_mm_free_slot*)ptr;
    p->next_free_slot = heap->free_slot[bin_num];
    heap->free_slot[bin_num] = p;
}

/********/
/* Heap */
/********/


static zend_always_inline void *zend_mm_alloc_heap(zend_mm_heap *heap, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	void *ptr;
	if (size <= ZEND_MM_MAX_SMALL_SIZE) {
		ptr = zend_mm_alloc_small(heap, size, ZEND_MM_SMALL_SIZE_TO_BIN(size) ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		return ptr;
	} else if (size <= ZEND_MM_MAX_LARGE_SIZE) {
		ptr = zend_mm_alloc_large(heap, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		return ptr;
	} else {
		return zend_mm_alloc_huge(heap, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	}
}

static zend_always_inline void zend_mm_free_heap(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t page_offset = ZEND_MM_ALIGNED_OFFSET(ptr, ZEND_MM_CHUNK_SIZE);

	if (UNEXPECTED(page_offset == 0)) {
		if (ptr != NULL) {
			zend_mm_free_huge(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		}
	} else {
		zend_mm_chunk *chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(ptr, ZEND_MM_CHUNK_SIZE);
		int page_num = (int)(page_offset / ZEND_MM_PAGE_SIZE);
		zend_mm_page_info info = chunk->map[page_num];

		ZEND_MM_CHECK(chunk->heap == heap, "zend_mm_heap corrupted");
		if (EXPECTED(info & ZEND_MM_IS_SRUN)) {
			zend_mm_free_small(heap, ptr, ZEND_MM_SRUN_BIN_NUM(info));
		} else /* if (info & ZEND_MM_IS_LRUN) */ {
			int pages_count = ZEND_MM_LRUN_PAGES(info);

			ZEND_MM_CHECK(ZEND_MM_ALIGNED_OFFSET(page_offset, ZEND_MM_PAGE_SIZE) == 0, "zend_mm_heap corrupted");
			zend_mm_free_large(heap, chunk, page_num, pages_count);
		}
	}
}

static size_t zend_mm_size(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t page_offset = ZEND_MM_ALIGNED_OFFSET(ptr, ZEND_MM_CHUNK_SIZE);

	if (UNEXPECTED(page_offset == 0)) {
		return zend_mm_get_huge_block_size(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	} else {
		zend_mm_chunk *chunk;
		int page_num;
		zend_mm_page_info info;

		chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(ptr, ZEND_MM_CHUNK_SIZE);
		page_num = (int)(page_offset / ZEND_MM_PAGE_SIZE);
		info = chunk->map[page_num];
		ZEND_MM_CHECK(chunk->heap == heap, "zend_mm_heap corrupted");
		if (EXPECTED(info & ZEND_MM_IS_SRUN)) {
			return bin_data_size[ZEND_MM_SRUN_BIN_NUM(info)];
		} else /* if (info & ZEND_MM_IS_LARGE_RUN) */ {
			return ZEND_MM_LRUN_PAGES(info) * ZEND_MM_PAGE_SIZE;
		}
	}
}

static void *zend_mm_realloc_heap(zend_mm_heap *heap, void *ptr, size_t size, size_t copy_size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t page_offset;
	size_t old_size;
	size_t new_size;
	void *ret;

	page_offset = ZEND_MM_ALIGNED_OFFSET(ptr, ZEND_MM_CHUNK_SIZE);
	if (UNEXPECTED(page_offset == 0)) {
		if (UNEXPECTED(ptr == NULL)) {
			return zend_mm_alloc_heap(heap, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		}
		old_size = zend_mm_get_huge_block_size(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		if (size > ZEND_MM_MAX_LARGE_SIZE) {
			new_size = ZEND_MM_ALIGNED_SIZE_EX(size, REAL_PAGE_SIZE);
			if (new_size == old_size) {
				zend_mm_change_huge_block_size(heap, ptr, new_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
				return ptr;
			} else if (new_size < old_size) {
				/* unmup tail */
				if (zend_mm_chunk_truncate(heap, ptr, old_size, new_size)) {
					heap->real_size -= old_size - new_size;
					heap->size -= old_size - new_size;
					zend_mm_change_huge_block_size(heap, ptr, new_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
					return ptr;
				}
			} else /* if (new_size > old_size) */ {
				if (UNEXPECTED(heap->real_size + (new_size - old_size) > heap->limit)) {
					if (zend_mm_gc(heap) && heap->real_size + (new_size - old_size) <= heap->limit) {
						/* pass */
					} else if (heap->overflow == 0) {
						zend_mm_safe_error(heap, "Allowed memory size of %zu bytes exhausted (tried to allocate %zu bytes)", heap->limit, size);
						return NULL;
					}
				}
				/* try to map tail right after this block */
				if (zend_mm_chunk_extend(heap, ptr, old_size, new_size)) {
					heap->real_size += new_size - old_size;
					heap->real_peak = MAX(heap->real_peak, heap->real_size);
					heap->size += new_size - old_size;
					heap->peak = MAX(heap->peak, heap->size);
					zend_mm_change_huge_block_size(heap, ptr, new_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
					return ptr;
				}
			}
		}
	} else {
		zend_mm_chunk *chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(ptr, ZEND_MM_CHUNK_SIZE);
		int page_num = (int)(page_offset / ZEND_MM_PAGE_SIZE);
		zend_mm_page_info info = chunk->map[page_num];

		ZEND_MM_CHECK(chunk->heap == heap, "zend_mm_heap corrupted");
		if (info & ZEND_MM_IS_SRUN) {
			int old_bin_num = ZEND_MM_SRUN_BIN_NUM(info);
			old_size = bin_data_size[old_bin_num];
			if (size <= ZEND_MM_MAX_SMALL_SIZE) {
				int bin_num = ZEND_MM_SMALL_SIZE_TO_BIN(size);
				if (old_bin_num == bin_num) {
					return ptr;
				}
			}
		} else /* if (info & ZEND_MM_IS_LARGE_RUN) */ {
			ZEND_MM_CHECK(ZEND_MM_ALIGNED_OFFSET(page_offset, ZEND_MM_PAGE_SIZE) == 0, "zend_mm_heap corrupted");
			old_size = ZEND_MM_LRUN_PAGES(info) * ZEND_MM_PAGE_SIZE;
			if (size > ZEND_MM_MAX_SMALL_SIZE && size <= ZEND_MM_MAX_LARGE_SIZE) {
				new_size = ZEND_MM_ALIGNED_SIZE_EX(size, ZEND_MM_PAGE_SIZE);
				if (new_size == old_size) {
					return ptr;
				} else if (new_size < old_size) {
					/* free tail pages */
					int new_pages_count = (int)(new_size / ZEND_MM_PAGE_SIZE);
					int rest_pages_count = (int)((old_size - new_size) / ZEND_MM_PAGE_SIZE);

					heap->size -= rest_pages_count * ZEND_MM_PAGE_SIZE;
					chunk->map[page_num] = ZEND_MM_LRUN(new_pages_count);
					chunk->free_pages += rest_pages_count;
					zend_mm_bitset_reset_range(chunk->free_map, page_num + new_pages_count, rest_pages_count);
					return ptr;
				} else /* if (new_size > old_size) */ {
					int new_pages_count = (int)(new_size / ZEND_MM_PAGE_SIZE);
					int old_pages_count = (int)(old_size / ZEND_MM_PAGE_SIZE);

					/* try to allocate tail pages after this block */
					if (page_num + new_pages_count <= ZEND_MM_PAGES &&
					    zend_mm_bitset_is_free_range(chunk->free_map, page_num + old_pages_count, new_pages_count - old_pages_count)) {
						do {
							size_t size = heap->size + (new_size - old_size);
							size_t peak = MAX(heap->peak, size);
							heap->size = size;
							heap->peak = peak;
						} while (0);
						chunk->free_pages -= new_pages_count - old_pages_count;
						zend_mm_bitset_set_range(chunk->free_map, page_num + old_pages_count, new_pages_count - old_pages_count);
						chunk->map[page_num] = ZEND_MM_LRUN(new_pages_count);
						return ptr;
					}
				}
			}
		}
	}

	/* Naive reallocation */
	do {
		size_t orig_peak = heap->peak;
		size_t orig_real_peak = heap->real_peak;
	ret = zend_mm_alloc_heap(heap, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	memcpy(ret, ptr, MIN(old_size, copy_size));
	zend_mm_free_heap(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		heap->peak = MAX(orig_peak, heap->size);
		heap->real_peak = MAX(orig_real_peak, heap->real_size);
	} while (0);
	return ret;
}

/*********************/
/* Huge Runs (again) */
/*********************/

static void zend_mm_add_huge_block(zend_mm_heap *heap, void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_huge_list *list = (zend_mm_huge_list*)zend_mm_alloc_heap(heap, sizeof(zend_mm_huge_list) ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	list->ptr = ptr;
	list->size = size;
	list->next = heap->huge_list;
	heap->huge_list = list;
}

static size_t zend_mm_del_huge_block(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_huge_list *prev = NULL;
	zend_mm_huge_list *list = heap->huge_list;
	while (list != NULL) {
		if (list->ptr == ptr) {
			size_t size;

			if (prev) {
				prev->next = list->next;
			} else {
				heap->huge_list = list->next;
			}
			size = list->size;
			zend_mm_free_heap(heap, list ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
			return size;
		}
		prev = list;
		list = list->next;
	}
	ZEND_MM_CHECK(0, "zend_mm_heap corrupted");
	return 0;
}

static size_t zend_mm_get_huge_block_size(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_huge_list *list = heap->huge_list;
	while (list != NULL) {
		if (list->ptr == ptr) {
			return list->size;
		}
		list = list->next;
	}
	ZEND_MM_CHECK(0, "zend_mm_heap corrupted");
	return 0;
}

static void zend_mm_change_huge_block_size(zend_mm_heap *heap, void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_huge_list *list = heap->huge_list;
	while (list != NULL) {
		if (list->ptr == ptr) {
			list->size = size;
			return;
		}
		list = list->next;
	}
}

static void *zend_mm_alloc_huge(zend_mm_heap *heap, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t new_size = ZEND_MM_ALIGNED_SIZE_EX(size, REAL_PAGE_SIZE);
	void *ptr;

	if (UNEXPECTED(heap->real_size + new_size > heap->limit)) {
		if (zend_mm_gc(heap) && heap->real_size + new_size <= heap->limit) {
			/* pass */
		} else if (heap->overflow == 0) {
			zend_mm_safe_error(heap, "Allowed memory size of %zu bytes exhausted (tried to allocate %zu bytes)", heap->limit, size);
			return NULL;
		}
	}
	ptr = zend_mm_chunk_alloc(heap, new_size, ZEND_MM_CHUNK_SIZE);
	if (UNEXPECTED(ptr == NULL)) {
		/* insufficient memory */
		if (zend_mm_gc(heap) &&
		    (ptr = zend_mm_chunk_alloc(heap, new_size, ZEND_MM_CHUNK_SIZE)) != NULL) {
			/* pass */
		} else {
			zend_mm_safe_error(heap, "Out of memory (allocated %zu) (tried to allocate %zu bytes)", heap->real_size, size);
			return NULL;
		}
	}
	zend_mm_add_huge_block(heap, ptr, new_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	do {
		size_t size = heap->real_size + new_size;
		size_t peak = MAX(heap->real_peak, size);
		heap->real_size = size;
		heap->real_peak = peak;
	} while (0);
	do {
		size_t size = heap->size + new_size;
		size_t peak = MAX(heap->peak, size);
		heap->size = size;
		heap->peak = peak;
	} while (0);
	return ptr;
}

static void zend_mm_free_huge(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t size;

	ZEND_MM_CHECK(ZEND_MM_ALIGNED_OFFSET(ptr, ZEND_MM_CHUNK_SIZE) == 0, "zend_mm_heap corrupted");
	size = zend_mm_del_huge_block(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	zend_mm_chunk_free(heap, ptr, size);
	heap->real_size -= size;
	heap->size -= size;
}

/******************/
/* Initialization */
/******************/

static zend_mm_heap *zend_mm_init(void)
{
	zend_mm_chunk *chunk = (zend_mm_chunk*)zend_mm_chunk_alloc_int(ZEND_MM_CHUNK_SIZE, ZEND_MM_CHUNK_SIZE);
	zend_mm_heap *heap;

	if (UNEXPECTED(chunk == NULL)) {
		fprintf(stderr, "\nCan't initialize heap: [%d] %s\n", errno, strerror(errno));
		return NULL;
	}
	heap = &chunk->heap_slot;
	chunk->heap = heap;
	chunk->next = chunk;
	chunk->prev = chunk;
	chunk->free_pages = ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE;
	chunk->free_tail = ZEND_MM_FIRST_PAGE;
	chunk->num = 0;
	chunk->free_map[0] = (Z_L(1) << ZEND_MM_FIRST_PAGE) - 1;
	chunk->map[0] = ZEND_MM_LRUN(ZEND_MM_FIRST_PAGE);
	heap->main_chunk = chunk;
	heap->cached_chunks = NULL;
	heap->chunks_count = 1;
	heap->peak_chunks_count = 1;
	heap->cached_chunks_count = 0;
	heap->avg_chunks_count = 1.0;
	heap->real_size = ZEND_MM_CHUNK_SIZE;
	heap->real_peak = ZEND_MM_CHUNK_SIZE;
	heap->size = 0;
	heap->peak = 0;
	heap->limit = ((size_t)Z_L(-1) >> (size_t)Z_L(1));
	heap->overflow = 0;
	heap->huge_list = NULL;
	return heap;
}

ZEND_API size_t zend_mm_gc(zend_mm_heap *heap)
{
	zend_mm_free_slot *p, **q;
	zend_mm_chunk *chunk;
	size_t page_offset;
	int page_num;
	zend_mm_page_info info;
	uint32_t i, free_counter;
	int has_free_pages;
	size_t collected = 0;


	for (i = 0; i < ZEND_MM_BINS; i++) {
		has_free_pages = 0;
		p = heap->free_slot[i];
		while (p != NULL) {
			chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(p, ZEND_MM_CHUNK_SIZE);
			ZEND_MM_CHECK(chunk->heap == heap, "zend_mm_heap corrupted");
			page_offset = ZEND_MM_ALIGNED_OFFSET(p, ZEND_MM_CHUNK_SIZE);
			ZEND_ASSERT(page_offset != 0);
			page_num = (int)(page_offset / ZEND_MM_PAGE_SIZE);
			info = chunk->map[page_num];
			ZEND_ASSERT(info & ZEND_MM_IS_SRUN);
			if (info & ZEND_MM_IS_LRUN) {
				page_num -= ZEND_MM_NRUN_OFFSET(info);
				info = chunk->map[page_num];
				ZEND_ASSERT(info & ZEND_MM_IS_SRUN);
				ZEND_ASSERT(!(info & ZEND_MM_IS_LRUN));
			}
			ZEND_ASSERT(ZEND_MM_SRUN_BIN_NUM(info) == i);
			free_counter = ZEND_MM_SRUN_FREE_COUNTER(info) + 1;
			if (free_counter == bin_elements[i]) {
				has_free_pages = 1;
			}
			chunk->map[page_num] = ZEND_MM_SRUN_EX(i, free_counter);;
			p = p->next_free_slot;
		}

		if (!has_free_pages) {
			continue;
		}

		q = &heap->free_slot[i];
		p = *q;
		while (p != NULL) {
			chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(p, ZEND_MM_CHUNK_SIZE);
			ZEND_MM_CHECK(chunk->heap == heap, "zend_mm_heap corrupted");
			page_offset = ZEND_MM_ALIGNED_OFFSET(p, ZEND_MM_CHUNK_SIZE);
			ZEND_ASSERT(page_offset != 0);
			page_num = (int)(page_offset / ZEND_MM_PAGE_SIZE);
			info = chunk->map[page_num];
			ZEND_ASSERT(info & ZEND_MM_IS_SRUN);
			if (info & ZEND_MM_IS_LRUN) {
				page_num -= ZEND_MM_NRUN_OFFSET(info);
				info = chunk->map[page_num];
				ZEND_ASSERT(info & ZEND_MM_IS_SRUN);
				ZEND_ASSERT(!(info & ZEND_MM_IS_LRUN));
			}
			ZEND_ASSERT(ZEND_MM_SRUN_BIN_NUM(info) == i);
			if (ZEND_MM_SRUN_FREE_COUNTER(info) == bin_elements[i]) {
				/* remove from cache */
				p = p->next_free_slot;;
				*q = p;
			} else {
				q = &p->next_free_slot;
				p = *q;
			}
		}
	}

	chunk = heap->main_chunk;
	do {
		i = ZEND_MM_FIRST_PAGE;
		while (i < chunk->free_tail) {
			if (zend_mm_bitset_is_set(chunk->free_map, i)) {
				info = chunk->map[i];
				if (info & ZEND_MM_IS_SRUN) {
					int bin_num = ZEND_MM_SRUN_BIN_NUM(info);
					int pages_count = bin_pages[bin_num];

					if (ZEND_MM_SRUN_FREE_COUNTER(info) == bin_elements[bin_num]) {
						/* all elemens are free */
						zend_mm_free_pages_ex(heap, chunk, i, pages_count, 0);
						collected += pages_count;
					} else {
						/* reset counter */
						chunk->map[i] = ZEND_MM_SRUN(bin_num);
					}
					i += bin_pages[bin_num];
				} else /* if (info & ZEND_MM_IS_LRUN) */ {
					i += ZEND_MM_LRUN_PAGES(info);
				}
			} else {
				i++;
			}
		}
		if (chunk->free_pages == ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE) {
			zend_mm_chunk *next_chunk = chunk->next;

			zend_mm_delete_chunk(heap, chunk);
			chunk = next_chunk;
		} else {
			chunk = chunk->next;
		}
	} while (chunk != heap->main_chunk);

	return collected * ZEND_MM_PAGE_SIZE;
}


void zend_mm_shutdown(zend_mm_heap *heap, int full, int silent)
{
	zend_mm_chunk *p;
	zend_mm_huge_list *list;



	/* free huge blocks */
	list = heap->huge_list;
	heap->huge_list = NULL;
	while (list) {
		zend_mm_huge_list *q = list;
		list = list->next;
		zend_mm_chunk_free(heap, q->ptr, q->size);
	}

	/* move all chunks except of the first one into the cache */
	p = heap->main_chunk->next;
	while (p != heap->main_chunk) {
		zend_mm_chunk *q = p->next;
		p->next = heap->cached_chunks;
		heap->cached_chunks = p;
		p = q;
		heap->chunks_count--;
		heap->cached_chunks_count++;
	}

	if (full) {
		/* free all cached chunks */
		while (heap->cached_chunks) {
			p = heap->cached_chunks;
			heap->cached_chunks = p->next;
			zend_mm_chunk_free(heap, p, ZEND_MM_CHUNK_SIZE);
		}
		/* free the first chunk */
		zend_mm_chunk_free(heap, heap->main_chunk, ZEND_MM_CHUNK_SIZE);
	} else {
		zend_mm_heap old_heap;

		/* free some cached chunks to keep average count */
		heap->avg_chunks_count = (heap->avg_chunks_count + (double)heap->peak_chunks_count) / 2.0;
		while ((double)heap->cached_chunks_count + 0.9 > heap->avg_chunks_count &&
		       heap->cached_chunks) {
			p = heap->cached_chunks;
			heap->cached_chunks = p->next;
			zend_mm_chunk_free(heap, p, ZEND_MM_CHUNK_SIZE);
			heap->cached_chunks_count--;
		}
		/* clear cached chunks */
		p = heap->cached_chunks;
		while (p != NULL) {
			zend_mm_chunk *q = p->next;
			memset(p, 0, sizeof(zend_mm_chunk));
			p->next = q;
			p = q;
		}

		/* reinitialize the first chunk and heap */
		old_heap = *heap;
		p = heap->main_chunk;
		memset(p, 0, ZEND_MM_FIRST_PAGE * ZEND_MM_PAGE_SIZE);
		*heap = old_heap;
		memset(heap->free_slot, 0, sizeof(heap->free_slot));
		heap->main_chunk = p;
		p->heap = &p->heap_slot;
		p->next = p;
		p->prev = p;
		p->free_pages = ZEND_MM_PAGES - ZEND_MM_FIRST_PAGE;
		p->free_tail = ZEND_MM_FIRST_PAGE;
		p->free_map[0] = (1L << ZEND_MM_FIRST_PAGE) - 1;
		p->map[0] = ZEND_MM_LRUN(ZEND_MM_FIRST_PAGE);
		heap->chunks_count = 1;
		heap->peak_chunks_count = 1;
		heap->real_size = ZEND_MM_CHUNK_SIZE;
		heap->real_peak = ZEND_MM_CHUNK_SIZE;
		heap->size = heap->peak = 0;
	}
}

/**************/
/* PUBLIC API */
/**************/

ZEND_API void* ZEND_FASTCALL _zend_mm_alloc(zend_mm_heap *heap, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return zend_mm_alloc_heap(heap, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void ZEND_FASTCALL _zend_mm_free(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	zend_mm_free_heap(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

void* ZEND_FASTCALL _zend_mm_realloc(zend_mm_heap *heap, void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return zend_mm_realloc_heap(heap, ptr, size, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

void* ZEND_FASTCALL _zend_mm_realloc2(zend_mm_heap *heap, void *ptr, size_t size, size_t copy_size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return zend_mm_realloc_heap(heap, ptr, size, copy_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API size_t ZEND_FASTCALL _zend_mm_block_size(zend_mm_heap *heap, void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return zend_mm_size(heap, ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

/**********************/
/* Allocation Manager */
/**********************/

typedef struct _zend_alloc_globals {
	zend_mm_heap *mm_heap;
} zend_alloc_globals;

# define AG(v) (alloc_globals.v)
static zend_alloc_globals alloc_globals;

ZEND_API int is_zend_mm(void)
{
	return 1;
}

#if defined(HAVE_BUILTIN_CONSTANT_P)
#undef _emalloc

# define ZEND_MM_CUSTOM_ALLOCATOR(size)
# define ZEND_MM_CUSTOM_DEALLOCATOR(ptr)

# define _ZEND_BIN_ALLOCATOR(_num, _size, _elements, _pages, x, y) \
	ZEND_API void* ZEND_FASTCALL _emalloc_ ## _size(void) { \
		ZEND_MM_CUSTOM_ALLOCATOR(_size); \
		return zend_mm_alloc_small(AG(mm_heap), _size, _num ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC); \
	}

ZEND_MM_BINS_INFO(_ZEND_BIN_ALLOCATOR, x, y)

ZEND_API void* ZEND_FASTCALL _emalloc_large(size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{

	ZEND_MM_CUSTOM_ALLOCATOR(size);
	return zend_mm_alloc_large(AG(mm_heap), size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void* ZEND_FASTCALL _emalloc_huge(size_t size)
{

	ZEND_MM_CUSTOM_ALLOCATOR(size);
	return zend_mm_alloc_huge(AG(mm_heap), size);
}

# define _ZEND_BIN_FREE(_num, _size, _elements, _pages, x, y) \
	ZEND_API void ZEND_FASTCALL _efree_ ## _size(void *ptr) { \
		ZEND_MM_CUSTOM_DEALLOCATOR(ptr); \
		{ \
			zend_mm_chunk *chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(ptr, ZEND_MM_CHUNK_SIZE); \
			ZEND_MM_CHECK(chunk->heap == AG(mm_heap), "zend_mm_heap corrupted"); \
			zend_mm_free_small(AG(mm_heap), ptr, _num); \
		} \
	}

ZEND_MM_BINS_INFO(_ZEND_BIN_FREE, x, y)

ZEND_API void ZEND_FASTCALL _efree_large(void *ptr, size_t size)
{

	ZEND_MM_CUSTOM_DEALLOCATOR(ptr);
	{
		size_t page_offset = ZEND_MM_ALIGNED_OFFSET(ptr, ZEND_MM_CHUNK_SIZE);
		zend_mm_chunk *chunk = (zend_mm_chunk*)ZEND_MM_ALIGNED_BASE(ptr, ZEND_MM_CHUNK_SIZE);
		int page_num = page_offset / ZEND_MM_PAGE_SIZE;
		uint32_t pages_count = ZEND_MM_ALIGNED_SIZE_EX(size, ZEND_MM_PAGE_SIZE) / ZEND_MM_PAGE_SIZE;

		ZEND_MM_CHECK(chunk->heap == AG(mm_heap) && ZEND_MM_ALIGNED_OFFSET(page_offset, ZEND_MM_PAGE_SIZE) == 0, "zend_mm_heap corrupted");
		ZEND_ASSERT(chunk->map[page_num] & ZEND_MM_IS_LRUN);
		ZEND_ASSERT(ZEND_MM_LRUN_PAGES(chunk->map[page_num]) == pages_count);
		zend_mm_free_large(AG(mm_heap), chunk, page_num, pages_count);
	}
}

ZEND_API void ZEND_FASTCALL _efree_huge(void *ptr, size_t size)
{

	ZEND_MM_CUSTOM_DEALLOCATOR(ptr);
	zend_mm_free_huge(AG(mm_heap), ptr);
}
#endif

ZEND_API void* ZEND_FASTCALL _emalloc(size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{

	return zend_mm_alloc_heap(AG(mm_heap), size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void ZEND_FASTCALL _efree(void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{

	zend_mm_free_heap(AG(mm_heap), ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void* ZEND_FASTCALL _erealloc(void *ptr, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{

	if (UNEXPECTED(AG(mm_heap)->use_custom_heap)) {
		if (ZEND_DEBUG && AG(mm_heap)->use_custom_heap == ZEND_MM_CUSTOM_HEAP_DEBUG) {
			return AG(mm_heap)->custom_heap.debug._realloc(ptr, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		} else {
			return AG(mm_heap)->custom_heap.std._realloc(ptr, size);
		}
	}
	return zend_mm_realloc_heap(AG(mm_heap), ptr, size, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void* ZEND_FASTCALL _erealloc2(void *ptr, size_t size, size_t copy_size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{

	if (UNEXPECTED(AG(mm_heap)->use_custom_heap)) {
		if (ZEND_DEBUG && AG(mm_heap)->use_custom_heap == ZEND_MM_CUSTOM_HEAP_DEBUG) {
			return AG(mm_heap)->custom_heap.debug._realloc(ptr, size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
		} else {
			return AG(mm_heap)->custom_heap.std._realloc(ptr, size);
		}
	}
	return zend_mm_realloc_heap(AG(mm_heap), ptr, size, copy_size ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API size_t ZEND_FASTCALL _zend_mem_block_size(void *ptr ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	if (UNEXPECTED(AG(mm_heap)->use_custom_heap)) {
		return 0;
	}
	return zend_mm_size(AG(mm_heap), ptr ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
}

ZEND_API void* ZEND_FASTCALL _safe_emalloc(size_t nmemb, size_t size, size_t offset ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return emalloc_rel(zend_safe_address_guarded(nmemb, size, offset));
}

ZEND_API void* ZEND_FASTCALL _safe_malloc(size_t nmemb, size_t size, size_t offset)
{
	return pemalloc(zend_safe_address_guarded(nmemb, size, offset), 1);
}

ZEND_API void* ZEND_FASTCALL _safe_erealloc(void *ptr, size_t nmemb, size_t size, size_t offset ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	return erealloc_rel(ptr, zend_safe_address_guarded(nmemb, size, offset));
}

ZEND_API void* ZEND_FASTCALL _safe_realloc(void *ptr, size_t nmemb, size_t size, size_t offset)
{
	return perealloc(ptr, zend_safe_address_guarded(nmemb, size, offset), 1);
}


ZEND_API void* ZEND_FASTCALL _ecalloc(size_t nmemb, size_t size ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	void *p;

	p = _safe_emalloc(nmemb, size, 0 ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	if (UNEXPECTED(p == NULL)) {
		return p;
	}
	memset(p, 0, size * nmemb);
	return p;
}

ZEND_API char* ZEND_FASTCALL _estrdup(const char *s ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	size_t length;
	char *p;

	length = strlen(s);
	if (UNEXPECTED(length + 1 == 0)) {
		zend_error_noreturn(E_ERROR, "Possible integer overflow in memory allocation (1 * %zu + 1)", length);
	}
	p = (char *) _emalloc(length + 1 ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	if (UNEXPECTED(p == NULL)) {
		return p;
	}
	memcpy(p, s, length+1);
	return p;
}

ZEND_API char* ZEND_FASTCALL _estrndup(const char *s, size_t length ZEND_FILE_LINE_DC ZEND_FILE_LINE_ORIG_DC)
{
	char *p;

	if (UNEXPECTED(length + 1 == 0)) {
		zend_error_noreturn(E_ERROR, "Possible integer overflow in memory allocation (1 * %zu + 1)", length);
	}
	p = (char *) _emalloc(length + 1 ZEND_FILE_LINE_RELAY_CC ZEND_FILE_LINE_ORIG_RELAY_CC);
	if (UNEXPECTED(p == NULL)) {
		return p;
	}
	memcpy(p, s, length);
	p[length] = 0;
	return p;
}


ZEND_API char* ZEND_FASTCALL zend_strndup(const char *s, size_t length)
{
	char *p;

	if (UNEXPECTED(length + 1 == 0)) {
		zend_error_noreturn(E_ERROR, "Possible integer overflow in memory allocation (1 * %zu + 1)", length);
	}
	p = (char *) malloc(length + 1);
	if (UNEXPECTED(p == NULL)) {
		return p;
	}
	if (EXPECTED(length)) {
		memcpy(p, s, length);
	}
	p[length] = 0;
	return p;
}


ZEND_API int zend_set_memory_limit(size_t memory_limit)
{
	AG(mm_heap)->limit = (memory_limit >= ZEND_MM_CHUNK_SIZE) ? memory_limit : ZEND_MM_CHUNK_SIZE;
	return SUCCESS;
}

ZEND_API size_t zend_memory_usage(int real_usage)
{
	if (real_usage) {
		return AG(mm_heap)->real_size;
	} else {
		size_t usage = AG(mm_heap)->size;
		return usage;
	}
	return 0;
}

ZEND_API size_t zend_memory_peak_usage(int real_usage)
{
	if (real_usage) {
		return AG(mm_heap)->real_peak;
	} else {
		return AG(mm_heap)->peak;
	}
	return 0;
}

ZEND_API void shutdown_memory_manager(int silent, int full_shutdown)
{
	zend_mm_shutdown(AG(mm_heap), full_shutdown, silent);
}

static void alloc_globals_ctor(zend_alloc_globals *alloc_globals)
{
	ZEND_TSRMLS_CACHE_UPDATE();
	alloc_globals->mm_heap = zend_mm_init();
}


ZEND_API void start_memory_manager(void)
{
	alloc_globals_ctor(&alloc_globals);
#  if defined(_SC_PAGESIZE)
	REAL_PAGE_SIZE = sysconf(_SC_PAGESIZE);
#  elif defined(_SC_PAGE_SIZE)
	REAL_PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
#  endif
}

ZEND_API zend_mm_heap *zend_mm_set_heap(zend_mm_heap *new_heap)
{
	zend_mm_heap *old_heap;

	old_heap = AG(mm_heap);
	AG(mm_heap) = (zend_mm_heap*)new_heap;
	return (zend_mm_heap*)old_heap;
}

ZEND_API zend_mm_heap *zend_mm_get_heap(void)
{
	return AG(mm_heap);
}

ZEND_API int zend_mm_is_custom_heap(zend_mm_heap *new_heap)
{
	return 0;
}

ZEND_API void zend_mm_set_custom_handlers(zend_mm_heap *heap,
                                          void* (*_malloc)(size_t),
                                          void  (*_free)(void*),
                                          void* (*_realloc)(void*, size_t))
{
}

ZEND_API void zend_mm_get_custom_handlers(zend_mm_heap *heap,
                                          void* (**_malloc)(size_t),
                                          void  (**_free)(void*),
                                          void* (**_realloc)(void*, size_t))
{
	*_malloc = NULL;
	*_free = NULL;
	*_realloc = NULL;
}


ZEND_API zend_mm_storage *zend_mm_get_storage(zend_mm_heap *heap)
{
	return NULL
}

ZEND_API zend_mm_heap *zend_mm_startup(void)
{
	return zend_mm_init();
}

ZEND_API zend_mm_heap *zend_mm_startup_ex(const zend_mm_handlers *handlers, void *data, size_t data_size)
{
	return NULL;
}

static ZEND_COLD ZEND_NORETURN void zend_out_of_memory(void)
{
	fprintf(stderr, "Out of memory\n");
	exit(1);
}

ZEND_API void * __zend_malloc(size_t len)
{
	void *tmp = malloc(len);
	if (EXPECTED(tmp)) {
		return tmp;
	}
	zend_out_of_memory();
}

ZEND_API void * __zend_calloc(size_t nmemb, size_t len)
{
	void *tmp = _safe_malloc(nmemb, len, 0);
	memset(tmp, 0, nmemb * len);
	return tmp;
}

ZEND_API void * __zend_realloc(void *p, size_t len)
{
	p = realloc(p, len);
	if (EXPECTED(p)) {
		return p;
	}
	zend_out_of_memory();
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 */
