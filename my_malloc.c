#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "my_malloc.h"
#include "printing.h"

/*
 * Due to the way assert() prints error messges we use out own assert function
 * for deteminism when testing assertions.
 */
#ifdef TEST_ASSERT
static inline void assert(int e) {
	if (!e) {
		const char *msg = "Assertion Failed!\n";
      	write(2, msg, strlen(msg));
      	exit(1);
	}
}
#else
  #include <assert.h>
#endif

// Mutex to ensure thread safety for the freelist.
static pthread_mutex_t mutex;

// Array of sentinel nodes for the freelists.
Header freelists[NUM_LISTS];

/*
 * Pointer to the second fencepost in the most recently allocated chunk from the OS.
 * Used for coalescing chunks.
 */
Header *lastFencepost;

/*
 * Pointer to maintian the base of the heap to allow printing based on the
 * distance from the base of the heap.
 */ 
void *base;

// List of chunks allocated by the OS for printing boundary tags.
Header *chunkList[MAX_NUM_CHUNKS];
size_t numChunks = 0;

/*
 * Direct the compiler to run the init function before running main.
 * This allows initialization of required globals.
 */
static void init (void) __attribute__ ((constructor));

// Helper functions for manipulating a pointer to a header.
static inline Header *getHeaderFromOffset(void *ptr, ptrdiff_t off);
static inline Header *getLeftHeader(Header *hdr);
static inline Header *ptrToHeader(void *ptr);

// Helper functions for allocating more memory from the OS.
static inline void initFencepost(Header *fp, size_t leftSize);
static inline void insertChunk(Header *hdr);
static inline void insertFenceposts(void *rawMem, size_t size);
static Header *allocChunk(size_t size);

// Helper function for freeing a block.
static inline void deallocObject(void *ptr);
// Helper function for allocating a block.
static inline Header *allocObject(size_t rawSize);

// Helper functions for verifying that the data structures are structurally valid.
static inline Header *detectCycles();
static inline Header *verifyPointers();
static inline bool verifyFreelist();
static inline Header *verifyChunk(Header *chunk);
static inline bool verifyTags();

static void init();

static bool isMallocInitialized;

// My Helpder functions for allocating and deallocating memory.
// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
static size_t _calcActualSize(size_t rawSize);
static int _getFreelistIndex(size_t actualSize);
static void *_coalesceChunks(Header *prevBlock, Header *currBlock);
static Header *_allocBlock(int index, size_t actualSize);
static bool _isEmptyFreelist(Header *hdr);
static bool _isSameIndex(int one, int other);
static void _updateBlock(Header *hdr);
static void _insertBlock(Header *hdr);
static void _removeBlock(Header *hdr);
// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

/*
 * @brief Helper function to retrieve a header pointer from a pointer and an 
 *        offset.
 *
 * @param ptr base pointer
 * @param off number of bytes from base pointer where header is located
 *
 * @return a pointer to a header offset bytes from pointer
 */
static inline Header *getHeaderFromOffset(void *ptr, ptrdiff_t off)
{
	return (Header *)((char *)ptr + off);
}

/*
 * @brief Helper function to get the header to the right of a given header.
 *
 * @param hdr original header
 *
 * @return header to the right of hdr
 */
Header *getRightHeader(Header *hdr)
{
	return getHeaderFromOffset(hdr, getSize(hdr));
}

/*
 * @brief Helper function to get the header to the left of a given header.
 *
 * @param hdr original header
 *
 * @return header to the right of hdr
 */
static inline Header *getLeftHeader(Header *hdr)
{
	return getHeaderFromOffset(hdr, -(hdr -> leftSize));
}

/*
 * @brief Fenceposts are marked as always allocated and may need to have
 * a left object size to ensure coalescing happens properly.
 *
 * @param fp a pointer to the Header being used as a fencepost
 * @param left_size the size of the object to the left of the fencepost
 */
static inline void initFencepost(Header *fp, size_t leftSize)
{
	setState(fp, FENCEPOST);
	setSize(fp, ALLOC_HEADER_SIZE);
	fp -> leftSize = leftSize;
}

/*
 * @brief Helper function to maintain list of chunks from the OS for debugging.
 *
 * @param hdr the first fencepost in the chunk allocated by the OS
 */
static inline void insertChunk(Header *hdr) 
{
	if (numChunks < MAX_NUM_CHUNKS) 
		chunkList[numChunks++] = hdr;
}

/*
 * @brief given a chunk of memory insert fenceposts at the left and 
 * right boundaries of the block to prevent coalescing outside of the
 * block.
 *
 * @param raw_mem a void pointer to the memory chunk to initialize
 * @param size the size of the allocated chunk
 */
static inline void insertFenceposts(void *rawMem, size_t size)
{
	// Convert to char * before performing operations.
 	char *mem = (char *)rawMem;

  	// Insert a fencepost at the left edge of the chunk.
  	Header *leftFencepost = (Header *)mem;
  	initFencepost(leftFencepost, ALLOC_HEADER_SIZE);

  	// Insert a fencepost at the right edge of the chunk.
  	Header *rightFencepost = getHeaderFromOffset(mem, size - ALLOC_HEADER_SIZE);
  	initFencepost(rightFencepost, size - 2 * ALLOC_HEADER_SIZE);
}

/*
 * @brief Allocate another chunk from the OS and prepare to insert it
 * into the freelist.
 *
 * @param size The size to allocate from the OS
 *
 * @return A pointer to the allocable block in the chunk (just after the 
 * first fencepost)
 */
static Header *allocChunk(size_t size) 
{
	void *mem = sbrk(size);
  
  	insertFenceposts(mem, size);
  	Header *hdr = (Header *)((char *)mem + ALLOC_HEADER_SIZE);
  	setState(hdr, UNALLOCATED);
  	setSize(hdr, size - 2 * ALLOC_HEADER_SIZE);
  	hdr -> leftSize = ALLOC_HEADER_SIZE;
  	return hdr;
}

// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
/*
 * @brief Compute the actual size of memory allocation from the raw size.
 *
 * @param rawSize Request size of a user
 *
 * @return An actual size to be allocated
 */
static size_t _calcActualSize(size_t rawSize)
{
	// Round up the raw size to be a multiple of 8-byte.
	rawSize = rawSize % MULTIPLE_8 == 0 ? rawSize : (((rawSize / MULTIPLE_8) + 1) * MULTIPLE_8);
	// Actual size = rounded request size + allocated metadata
	size_t actualSize = ALLOC_HEADER_SIZE + rawSize; 
	// Minimum allocation size = unallocated header size
	actualSize = actualSize > UNALLOC_HEADER_SIZE ? actualSize : UNALLOC_HEADER_SIZE;

	return actualSize;
}

/*
 * @brief Compute an index of a freelist to get a block for allocation based on the actual size.
 * 
 * @param actualSize The actual size of memory allocation.
 *
 * @return An index of an appropriate freelist
 */
static int _getFreelistIndex(size_t actualSize)
{
	// A block in a freelist excludes metadata.
	int index = ((actualSize - ALLOC_HEADER_SIZE) / MULTIPLE_8) - 1;
	// Index must be smaller than the NUM_LISTS.
	return index < NUM_LISTS ? index : NUM_LISTS - 1;
}

/*
 * @brief Determine if a freelist is empty.
 *
 * @param hdr A header
 *
 * @return True if empty, false otherwise
 */
static bool _isEmptyFreelist(Header *hdr)
{
	// Sentinel is a dummy node.
	return hdr -> next == hdr;
}

/*
 * @brief Determine if two indexs are the same.
 *
 * @param one An index, other The other index
 *
 * @return True if same, false otherwise
 */
static bool _isSameIndex(int one, int other)
{
	return one == other;
}

/*
 * @brief Insert a block at the beginning of a freelist.
 *
 * @param hdr a header
 */
static void _insertBlock(Header *hdr)
{
	int index = _getFreelistIndex(getSize(hdr));
	Header *sentinel = &freelists[index];

	if (!_isEmptyFreelist(sentinel)) {
		sentinel -> next -> prev = hdr;
		hdr -> next = sentinel -> next;
	} else {
		sentinel -> prev = hdr;
		hdr -> next = sentinel;
	}

	sentinel -> next = hdr;
	hdr -> prev = sentinel;
}

/*
 * @brief Remove a block from a freelist.
 *
 * @param hdr a header
 */
static void _removeBlock(Header *hdr)
{
	hdr -> prev -> next = hdr -> next;
	hdr -> next -> prev = hdr -> prev;
}

/*
 * @brief Update a block from a freelist.
 *
 * @param hdr a header
 */
static void _updateBlock(Header *hdr)
{
	_removeBlock(hdr);
	_insertBlock(hdr);
}

/*
 * @brief Coalesce two distinct chunks.
 *
 * @param prevBlock A header to the left of the currBlock, currBlock A header of the current block
 */
static void *_coalesceChunks(Header *prevBlock, Header *currBlock)
{
	// Get the 2nd fencepost of the previous chunk.
	Header *prevSecondFencepost = getRightHeader(prevBlock); 
	// Get the 2nd fencepost of the current chunk.
	Header *currSecondFencepost = getRightHeader(currBlock);

	size_t index;
	size_t coalescedSize;
	// If the previous  block is unallocated.
	if (getState(prevBlock) == UNALLOCATED) {	
		index = _getFreelistIndex(getSize(prevBlock));
		// Coalesce two chunks.(previous block + current block + 2 * fencepost)
		// Update the size, left size.
		coalescedSize = getSize(prevBlock) + getSize(currBlock) + 2 * ALLOC_HEADER_SIZE;
		setSize(prevBlock, coalescedSize);

		// If the coalesced chunk does not fit, insert into an appropriate freelist.
		if (index != _getFreelistIndex(getSize(prevBlock)))
			_updateBlock(prevBlock);
	// If the previous block is allocated.
	} else { 	
		// Coalesce the two chunks.(current block + 2 * fencepost)
		// Update the size, left size.
		coalescedSize = getSize(currBlock) + 2 * ALLOC_HEADER_SIZE;
		setSize(prevSecondFencepost, coalescedSize);
		setState(prevSecondFencepost, UNALLOCATED);
		// Fencepost is now a header, so there is no need to remove the block.
		_insertBlock(prevSecondFencepost);
	}

	// Update the last fencepost (global variable).
	currSecondFencepost -> leftSize = coalescedSize;
}

/*
 * @brief Allocate a block based on a freelist index and an actual size.
 *
 * @param index An index of a freelist where a block resides, actualSize An actual size of memory allocation
 *
 * @return A header to a block
 */
static Header *_allocBlock(int index, size_t actualSize)
{
	for (int i = index; i < NUM_LISTS; i++) {
		// Get a freelist at index i.
		Header *sentinel = &freelists[i];
		
		// Search only an non-empty freelist.
		// Exception: final freelist (i.e. i == NUM_LISTS - 1), iterate over the freelist.
		if (sentinel == sentinel -> next && i != NUM_LISTS - 1)
			continue;	

		for (Header *curr = sentinel -> next; curr != sentinel; curr = curr -> next) {
			size_t currSize = getSize(curr);
			size_t newIndex;
			// If current block size equals actual size or the difference is smaller than unallocated header size. 
			// Why unallocated header? Minimum allocation size equals unallocated header size.
			// Remove it from the freelist.
			if ((currSize == actualSize) || ((currSize - actualSize) < UNALLOC_HEADER_SIZE)) {
				setState(curr, ALLOCATED);
				_removeBlock(curr);

				return (Header *)(curr -> data);
			// If the difference exceeds the unallocated header size, split the current block into two smaller blocks.
			// Allocate the rightmost block.
			} else {
				// Split the current block into two smaller blocks.
				setSize(curr, currSize - actualSize);
				currSize = getSize(curr);
				
				// Update the right block.
				Header *rightBlock = getRightHeader(curr); 
				setSize(rightBlock, actualSize);
				setState(rightBlock, ALLOCATED);
				rightBlock -> leftSize = currSize;
			
				// Update the right block's next block.
				Header *nextBlock = getRightHeader(rightBlock); 
				nextBlock -> leftSize = actualSize;

				// If the remaining block does not fit in the current freelist.
				// Remove and put it into the appropriate freelist.
				newIndex = _getFreelistIndex(currSize);
				if (!_isSameIndex(newIndex, i))
					_updateBlock(curr);

				return (Header *)(rightBlock -> data);
			}
		}	

		// No available block large enough to fit the actual size.
		// Allocate a new chunk of memory from the OS.
		Header *currBlock = allocChunk(ARENA_SIZE);
		size_t currBlockSize = getSize(currBlock);
		Header *currFirstFencepost = getLeftHeader(currBlock); 
		Header *currSecondFencepost = getRightHeader(currBlock);

		// Previous block.
		Header *prevSecondFencepost = getHeaderFromOffset(currFirstFencepost, -ALLOC_HEADER_SIZE);
		Header *prevBlock = getLeftHeader(prevSecondFencepost);
	
		// The previous block and current block are contiguous. Coalesce two chunks.
		if (prevSecondFencepost == lastFencepost) {
			_coalesceChunks(prevBlock, currBlock);				
		// The previous block and current block are not contiguous. Insert a new chunk.
		} else {
			// Insert a chunk into the chunk list.
  			 insertChunk(currFirstFencepost);
			_insertBlock(currBlock);
		}
		
		// Update the last fencepost.
		lastFencepost = currSecondFencepost;

		return _allocBlock(index, actualSize);
	}
}
// @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@

static inline Header *allocObject(size_t rawSize) 
{
	if (rawSize == 0)
		return NULL;

	// Compute an actual size from a raw size.
	size_t actualSize = _calcActualSize(rawSize);
	// Get an Index of a freelist.
	int index = _getFreelistIndex(actualSize);
	// Allocate a block from a freelist.
	return _allocBlock(index, actualSize);
}

/*
 * @brief Helper to get the Header from a pointer allocated with malloc
 *
 * @param p pointer to the data region of the block
 *
 * @return A pointer to the Header of the block
 */
static inline Header *ptrToHeader(void *ptr)  
{
	return (Header *)((char *)ptr - ALLOC_HEADER_SIZE); //sizeof(Header));
}

static inline void deallocObject(void *ptr)
{
	if (ptr == NULL) 
		return;
	
	// Get a current block, previous block, and next block.
	Header *currBlock = ptrToHeader(ptr);
	Header *prevBlock = getLeftHeader(currBlock);
	Header *nextBlock = getRightHeader(currBlock);

	// Get states for the previous block and the next block.	
	State prevState = getState(prevBlock);
	State nextState = getState(nextBlock);

	// If the block has already been deallocated, freeing it again generates an error.
	if (getState(currBlock) == UNALLOCATED) {
		fprintf(stderr, "Double Free Detected\n");
		fprintf(stderr, "Assertion Failed!\n");
		exit(1);
	}
	
	// Set the state of the current block unallocated.
	setState(currBlock, UNALLOCATED);
	size_t size;
	int index;
	int newIndex;

	// Both neighboring blocks are unallocated.
	if (prevState == UNALLOCATED && nextState == UNALLOCATED) {
		index = _getFreelistIndex(getSize(prevBlock));
		size = getSize(prevBlock) + getSize(currBlock) + getSize(nextBlock);
		setSize(prevBlock, size);

		Header *nextNextBlock = getRightHeader(nextBlock);
		nextNextBlock -> leftSize = size;

		// If the coalseced size is not fit for the current freelist where the prevBlock is stored.
		// Insert it into an appropriate freelist.
		newIndex = _getFreelistIndex(getSize(prevBlock));
		if (!_isSameIndex(index, newIndex))
			_updateBlock(prevBlock);

	// Only the prev block is unallocated.
	// Coalesce the current block and the previous block.
	} else if (prevState == UNALLOCATED) {
		index = _getFreelistIndex(getSize(prevBlock));
		size = getSize(prevBlock) + getSize(currBlock);
		setSize(prevBlock, size);
		nextBlock -> leftSize = size;
		
		// If the coalseced size is not fit for the freelist where the prevBlock is stored.
		// Insert it into an appropriate freelist.
		newIndex = _getFreelistIndex(getSize(prevBlock));
		if (!_isSameIndex(index, newIndex))
			_updateBlock(prevBlock);

	// Only the next block is unallocated.
	// Coalesce the current block and the next block.
	} else if (nextState == UNALLOCATED) {
		index = _getFreelistIndex(getSize(nextBlock));
		size = getSize(nextBlock) + getSize(currBlock);	
		setSize(currBlock, size);

		Header *nextNextBlock = getRightHeader(nextBlock);
		nextNextBlock -> leftSize = size;

		// If the coalseced size is not fit for the freelist where the nextBlock is stored.
		// Insert it into an appropriate freelist.
		newIndex = _getFreelistIndex(getSize(currBlock));
		if (!_isSameIndex(index, newIndex))
			_updateBlock(currBlock);
	
	// Both neighboring blocks are allocated.
	// Insert into an appropriate freelist.
	} else {
		_insertBlock(currBlock);	
	}	
}

/*
 * @brief Helper to detect cycles in the free list.
 * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
 *
 * @return One of the nodes in the cycle or NULL if no cycle is present.
 */
static inline Header *detectCycles()
{
	for (int i = 0; i < NUM_LISTS; i++) {
    	Header *sentinel = &freelists[i];
    	for (Header *slow = sentinel->next, *fast = sentinel -> next -> next; 
			fast != sentinel; 
			slow = slow -> next, fast = fast -> next -> next) {
			if (slow == fast)
        		return slow;
		}
	}

	return NULL;
}

/*
 * @brief Helper to verify that there are no unlinked previous or next pointers
 *        in the free list.
 *
 * @return A node whose previous and next pointers are incorrect or NULL if no
 *         such node exists.
 */
static inline Header *verifyPointers()
{
	for (int i = 0; i < NUM_LISTS; i++) {
    	Header *freelist = &freelists[i];
    	for (Header *curr = freelist -> next; curr != freelist; curr = curr -> next) {
      		if (curr -> next -> prev != curr || curr -> prev -> next != curr)
        		return curr;
    	}
  	}
	
	return NULL;
}

/*
 * @brief Verify the structure of the free list is correct by checkin for 
 *        cycles and misdirected pointers.
 *
 * @return true if the list is valid
 */
static inline bool verifyFreelist()
{
	Header *cycle = detectCycles();
	if (cycle != NULL) {
		fprintf(stderr, "Cycle Detected\n");
    	print_sublist(print_object, cycle -> next, cycle);
    	return false;
	}
  
	Header *invalid = verifyPointers();
  	if (invalid != NULL) {
		fprintf(stderr, "Invalid pointers\n");
    	print_object(invalid);
    	return false;
  	}

	return true;
}

/*
 * @brief Helper to verify that the sizes in a chunk from the OS are correct
 *        and that allocated node's canary values are correct.
 *
 * @param chunk AREA_SIZE chunk allocated from the OS
 *
 * @return a pointer to an invalid Header or NULL if all Header's are valid
 */
static inline Header *verifyChunk(Header *chunk)
{
	if (getState(chunk) != FENCEPOST) {
		fprintf(stderr, "Invalid fencepost\n");
		print_object(chunk);
		return chunk;
	}
	
	for (; getState(chunk) != FENCEPOST; chunk = getRightHeader(chunk)) {
		if (getSize(chunk)  != getRightHeader(chunk) -> leftSize) {
			fprintf(stderr, "Invalid sizes\n");
			print_object(chunk);
			return chunk;
		}
	}
	
	return NULL;
}

/*
 * @brief For each chunk allocated by the OS verify that the boundary tags
 *        are consistent.
 *
 * @return true if the boundary tags are valid
 */
static inline bool verifyTags()
{
	for (size_t i = 0; i < numChunks; i++) {
		Header *invalid = verifyChunk(chunkList[i]);
		if (invalid != NULL)
			return invalid;
	}

	return NULL;
}

/**
 * @brief Initialize mutex lock and prepare an initial chunk of memory for allocation.
 */
static void init()
{
	// Initialize mutex for thread safety
  	pthread_mutex_init(&mutex, NULL);

#ifdef DEBUG
  	// Manually set printf buffer so it won't call malloc when debugging the allocator.
  	setvbuf(stdout, NULL, _IONBF, 0);
#endif // DEBUG

	// Allocate the first chunk from the OS
  	Header *block = allocChunk(ARENA_SIZE);
  	// Compute the address of the previous fencepost.
  	Header *prevFencepost = getHeaderFromOffset(block, -ALLOC_HEADER_SIZE);
  	// Insert a newly allocated memory chuck by the OS into the list of chunks.
  	insertChunk(prevFencepost);
  	// Get the last fencepost of the chunk.
  	lastFencepost = getHeaderFromOffset(block, getSize(block));

  	// Set the base pointer to the beginning of the first fencepost in the first
  	// chunk from the OS.
  	base = ((char *)block) - ALLOC_HEADER_SIZE; //sizeof(Header);

  	// Initialize freelist sentinels.
  	for (int i = 0; i < NUM_LISTS; i++) {
		Header *sentinel = &freelists[i];
    	sentinel -> next = sentinel;
    	sentinel -> prev = sentinel;
	}

  	// Insert the first chunk into a free list.
  	Header *sentinel = &freelists[NUM_LISTS - 1];
  	sentinel -> next = block;
  	sentinel -> prev = block;
  	block -> next = sentinel;
  	block -> prev = sentinel;
}

void *myMalloc(size_t size)
{
	pthread_mutex_lock(&mutex);
	Header *block = allocObject(size);
	pthread_mutex_unlock(&mutex);

	return block;
}

void *myCalloc(size_t nmemb, size_t size)
{
	return memset(myMalloc(size * nmemb), 0, size * nmemb);
}

void *myRealloc(void *ptr, size_t size)
{
	void *reallocMem = myMalloc(size);
	memcpy(reallocMem, ptr, size);
	myFree(ptr);

	return reallocMem;
}

void myFree(void *ptr)
{
	pthread_mutex_lock(&mutex);
	deallocObject(ptr);
	pthread_mutex_unlock(&mutex);
}

bool verify()
{
	return verifyFreelist() && verifyTags();
}
