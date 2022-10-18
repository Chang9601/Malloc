#ifndef __MY_MALLOC_H__
#define __MY_MALLOC_H__

#include <stdbool.h>
#include <sys/types.h>

#define RELATIVE_POINTERS true

#ifndef ARENA_SIZE
// If not specified at compile time use the default arena size
#define ARENA_SIZE 4096
#endif

#ifndef NUM_LISTS
// If not specified at compile time use the default number of free lists
#define NUM_LISTS 59
#endif

/*
 * Size of the header for an allocated block = 16
 * The size of the normal minus the size of the two free list pointers as
 * they are only maintained while block is free
 */
#define ALLOC_HEADER_SIZE (sizeof(Header) - (2 * sizeof(Header *)))

/* The minimum size request the allocator will service */
#define MIN_ALLOCATION 8

/* Block sizes = multiple of 8 */
#define MULTIPLE_8 8

/* Size of the header for an unallocated block = 32 */
#define UNALLOC_HEADER_SIZE sizeof(Header)

/**
 * @brief enum representing the allocation state of a block
 *
 * enums provice a method of specifying a set of named values
 * http://en.cppreference.com/w/c/language/enum
 */
typedef enum {
	UNALLOCATED = 0,
  	ALLOCATED = 1,
  	FENCEPOST = 2,
} State;

/*
 * The Header contains all metadata about a block to be allocated
 * The size fields allow accessing the neighboring blocks in memory by
 * calculating their stating and ending addresses.
 *
 * When a block is free the free list pointer fields are set to the next
 * and previous blocks in the free list to allow faster traversal of the
 * freed blocks
 *
 * When a block is allocated the user's data starts by using the 16 bytes
 * reserved for the freelist pointers
 *
 * The zero length array at the end of the struct is the beginning of the
 * usable data in the block.
 *
 * FIELDS ALWAYS PRESENT
 * size_t size: The size of the current block *including metadata*
 * size_t left_size: The size of the block to the left (in memory)
 *
 * FIELDS PRESENT WHEN FREE
 * Header *next: The next block in the free list (only valid if free)
 * Header *prev: The previous block in the free list (only valid if free)
 *
 * FIELD PRESENT WHEN ALLOCATED
 * size_t[] canary: magic value to detetmine if a block has been corrupted
 *
 * char[] data: first byte of data pointed to by the list
 */
typedef struct _Header {
	size_t size;
  	size_t leftSize;
  	union {
    	// Used when the object is free
    	struct {
      		struct _Header *next;
      		struct _Header *prev;
    	};
    	// Used when the object is allocated
    	char data[0];
	};
} Header;

// Helper functions for getting and storing size and state from Header
// Since the size is a multiple of 8, the last 3 bits are always 0s. -> ??
// Therefore we use the 3 lowest bits to store the state of the object.
// This is going to save 8 bytes in all objects.

static inline size_t getSize(Header *hdr)
{
	return hdr -> size & ~0x3;
}

static inline void setSize(Header *hdr, size_t size)
{
	hdr -> size = size | (hdr -> size & 0x3);
}

static inline State getState(Header *hdr)
{
	return (State) (hdr -> size & 0x3);
}

static inline void setState(Header *hdr, State state)
{
	hdr -> size = (hdr -> size & ~0x3) | state;
}

static inline void setSizeAndState(Header *hdr, size_t size, State state)
{
	hdr -> size = (size & ~0x3) | (state & 0x3);
}

#define MAX_NUM_CHUNKS 1024

// Malloc interface
void *myMalloc(size_t size);
void *myCalloc(size_t nmemb, size_t size);
void *myRealloc(void *ptr, size_t size);
void myFree(void *ptr);

// Debug list verifitcation
bool verify();

// Helper to find a block's right neighbor
Header *getRightHeader(Header *hdr);

/*
 * Global variables used in malloc that are needed by other C files
 *
 * extern tells the compiler that the variables exist in another file and
 * will be present when the final binary is linked
 */
extern void *base;
extern Header freelists[];
extern char freelistBitmap[];
extern Header *chunkList[];
extern size_t numChunks;

#endif // __MY_MALLOC_H__
