#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "sfmm.h"

/* Minimum block size */
#define MIN_BLOCK_SIZE 32
/* One memory row is 8 bytes */
#define MROW 8
#define HEAP_SIZE() (sf_mem_end() - sf_mem_start()) /* Return the heap size calculated from difference in starting and end address */
#define QL_MAX_SIZE 224 // 32 + 16 * 12 = 224 bytes size for the last quick list (EXCLUSIVE)
#define QL_INDEX(size) (size-32)/16 /* Return calculated quick list index based on size passed in (note: size should always be a multiple of 16 */
#define PROLOGUE_SIZE 32
#define EPILOGUE_SIZE 8
// Construct the size variable based on the parameters passed in
#define PACK(pl_size, block_size, in_ql, alloc) (size_t) (((size_t)pl_size << 32) | (block_size) | (in_ql << 1) | (alloc))
#define GET_PL_SIZE(header) (header >> 32)
#define GET_BLOCK_SIZE(header) (((size_t)header) & ~0xFFFFFFFF0000000F)
// Obfuscate macro (simply XOR)
#define OBF(value) ((value) ^ MAGIC)

// Macros to grab next and previous blocks based on pointer passed in
#define NEXT_BLOCK(block) (sf_block *) ((char *) block + GET_BLOCK_SIZE(OBF(block -> header)))
#define PREV_BLOCK(block) (sf_block *) ((char *) block - GET_BLOCK_SIZE(OBF(*((sf_footer *)( (char *)block - MROW )))))

// Define footer macro for a given block pointer (sf_block *)
#define FOOTER(block) (sf_footer *) ((char *) block + GET_BLOCK_SIZE(OBF(block -> header)) - MROW) 

sf_block* split_malloc_block(sf_block* block, size_t block_size, size_t pl_size);
void *create_malloc_block(sf_block* free_block, size_t pl_size);
int validate_pp(void * pp);
void unlink_block(sf_block *block);
sf_block* popQL(int index);
sf_block* find_fit(size_t block_size);
int initialize_heap();
int extend_heap();
int get_ml_index(size_t size);
void *create_free_block(size_t block_size, char *start_addr);
void initialize_free_lists();
void insert_ml(sf_block* free_block);
void insert_ql(sf_block * free_block);
void flush_ql(int index);
sf_block *coalesce(sf_block* free_block);
sf_block* split_free_block(sf_block* free_block, size_t block_size);

// Variables to track statistics for sf_util
// Current running total
size_t running_pl = 0;
// Max total
size_t max_pl = 0;
/**
* @brief Simple helper function to update max and running total
* @param size, increment/decrement to add/subtract from running_pl 
*/
void update_pl(size_t size){
    running_pl += size;
    if(running_pl > max_pl) max_pl = running_pl;
}


void *sf_malloc(size_t size) {
    // Check if size is 0, return NULL in this case
    if (size == 0)
        return NULL;
    // Check if heap_size is large enough to store the requested size
    size_t heap_size = HEAP_SIZE();

    // Check if heap_size is 0, if so, then initialize
    if(heap_size == 0) {
        initialize_free_lists();
        int ret = initialize_heap();
        if(ret) return NULL;
    }
    // printf("heap and free lists have been INITIALIZED SKIBIDi!!!!\n");

    // Variable to store total block size (including padding and footer/header and everything)
    size_t block_size = size + 2*MROW; // Adding 2 memory rows of space for header and footer 
    // Check edge cases
    // 1. if block_size is < 32
    if(block_size < 32) block_size = 32;

    // 2. if block_size is not 16-byte aligned
    // Add however much padding is needed
    if((block_size % 16) != 0) block_size += (16 - block_size % 16); 
    // printf("Block size: %zu\n", block_size);
    // Now, check if quick_lists should be searched or main lists, based on block_size
    if (block_size < QL_MAX_SIZE) {
        // Get QL index
        int index = QL_INDEX(block_size);
        // Pop from QL at index
        sf_block *block = popQL(index);
        // continue only if sf_block isn't NULL
        if(block) {
            // Allocate the block
            char *pp = create_malloc_block(block, size);
            update_pl(size); 
            return pp;
        }
    }
    // Now, implement checking main list whenever the size is either too large for quick list
    // when the corresponding quick list is empty
    // Note: Since the new memory will coalesce with the old, there's no edge case like needing to check the quicklist since 
    // there's no way for anything to be stored into quicklist when extending the heap.
    sf_block *fit_block = NULL;
    do {
        // Find a block that fits the block size
        fit_block = find_fit(block_size); 
        // printf("after finding fit block\n");
        // If fit_block is null, extend heap an continue to next iteration
        if(!fit_block) {
            // printf("extending heap\n");
            int ret = extend_heap();
            // If ret is -1, that means no more space, return NULL
            if(ret) return NULL;
            // else, continue
            continue;
        }

        // Else, unlink the block, effectively removing it from the main list
        unlink_block(fit_block);
    } while(!fit_block);
    // Now that fit_block has been grabbed, split as needed and then return that block of memory
    // Remember: block_size is the minimum size needed for the size passed in, the fit_block size can be >= to this
    fit_block = split_free_block(fit_block, block_size);
    
    // Create allocated block from fit_block
    char* pp = create_malloc_block(fit_block, size); 
    update_pl(size); 
    return pp;
}

/**
 * Note for self:
 *   *pp points to the payload, not the header
 */
void sf_free(void *pp) {
    // Validate pointer
    int ret = validate_pp(pp);
    if(ret) abort();

    // Grab header
    sf_header *hPtr = (sf_header *)((char*) pp - MROW);
    sf_header header = (sf_header) OBF(*hPtr);
    // Grab block size
    size_t block_size = GET_BLOCK_SIZE(header);
    // Grab pl_size for updating the running and max
    size_t pl_size = GET_PL_SIZE(header);

    // Create free block starting at the header with the given block size 
    sf_block * free_block = create_free_block(block_size, (char *)hPtr); 
    // Coalesce the free block
    free_block = coalesce(free_block);
    // Grab new block size
    block_size = GET_BLOCK_SIZE(OBF(free_block -> header));

    // Based on block_size, insert into corresponding list
    if(block_size < QL_MAX_SIZE) insert_ql(free_block);
    else insert_ml(free_block);

    // update the running total by the negative
    update_pl(-pl_size);
}
/*
 * Resizes the memory pointed to by ptr to size bytes.
 *
 * @param ptr Address of the memory region to resize.
 * @param size The minimum size to resize the memory to.
 *
 * @return If successful, the pointer to a valid region of memory is
 * returned, else NULL is returned and sf_errno is set appropriately.
 *
 *   If sf_realloc is called with an invalid pointer sf_errno should be set to EINVAL.
 *   If there is no memory available sf_realloc should set sf_errno to ENOMEM.
 *
 * If sf_realloc is called with a valid pointer and a size of 0 it should free
 * the allocated block and return NULL without setting sf_errno.
 */
void *sf_realloc(void *pp, size_t rsize) {
    // validate pp
    int ret = validate_pp(pp);
    // If invalid, set sf_errno to EINVAL and return null
    if(ret) {
        sf_errno = EINVAL;
        return NULL;
    }

    // If valid, check size, if 0, free block and return NUL
    if(rsize == 0) {
        sf_free(pp);
        return NULL;
    }
    // Grab header
    sf_header * hPtr = (sf_header *) ((char *)pp - MROW);
    sf_header header = (sf_header) OBF(*hPtr);
    // Grab block size
    size_t block_size = GET_BLOCK_SIZE(header);
    // Grab payload size (for memcpy)
    size_t pl_size = GET_PL_SIZE(header);
    // printf("after grabbing stuff from header, block size: %lu, rsize: %lu\n", block_size, rsize);
    // fflush(stdout);
    // Case 0: reallocating to same size (for some reason)
    if(rsize == pl_size) return pp;
    
    // Pointer to return
    // char *ptr = NULL;
    // Case 1: reallocating to larger size
    if (pl_size < rsize) {
        char *ptr = sf_malloc(rsize);
        // Error handle, if ptr is NULL, just return NULL, sf_errno should be set by malloc
        if(!ptr) return NULL;

        // Else memcpy payload over (note that pp is the beginning address of the payload)
        memcpy(ptr, pp, pl_size);

        // Now free the old block 
        sf_free(pp); 

        // Update running total by difference
        update_pl(rsize - pl_size);
        // Return new pointer
        return ptr; 
    }
    // Case 2: reallocating to smaller size
    else {
        // In this case, the existing block should be used and split

        // Grab size with padding. rsize will be the payload size and block_size will be the total  block size with padding (header + footer + padding)
        block_size = (rsize + 2 * MROW);
        if(block_size % 16 != 0) block_size += 16 - (rsize % 16);

        // Now, simply pass block pointer (header pointer) to split_malloc_block method.
        sf_block *block = split_malloc_block((sf_block *)hPtr, block_size, rsize);
        
        // Update running and max
        update_pl(rsize - pl_size);

        // Set header and footer of free_block
        char *ptr = (char*)block + MROW;
        return ptr;
    } 
}
/**
 * @brief returns total amount of internal fragmentation which is total amount of payload / total size of allocated blocks
 */
double sf_fragmentation() {
   // initialize total payload
   size_t total_pl = 0;
   // initialize total size
   size_t total_size = 0;
   
    // iterate through the heap, first grab start of heap then get first block by adding 5 memory rows
    sf_block *curBlock = (sf_block*)((char*)sf_mem_start() + 5 * MROW);
    // Prologue so that while loop knows when to stop
    sf_block *prologue = (sf_block*)((char*)sf_mem_end() - MROW);

    while(curBlock != prologue) {
        // printf("cur block in while\n");
        // sf_show_block(curBlock);
        // Grab header of cur block
        sf_header header = OBF(curBlock -> header);
        // grab block size
        size_t block_size = GET_BLOCK_SIZE(header);
        // grab allocation bit
        int alloc = header & THIS_BLOCK_ALLOCATED;
        // grab quicklist bit
        int ql = header & IN_QUICK_LIST;

        // Make sure it's not in quick list and is allocated
        if(ql || !alloc) {
            // go to next block
            curBlock = (sf_block*)((char*) curBlock + block_size);
            continue;
        } 
        // else add to total block and pl size
        total_size += block_size;
        size_t pl = GET_PL_SIZE(header);
        total_pl += pl; 
        // increment to next block
        curBlock = (sf_block*)((char*) curBlock + block_size);
    }
    // check if no allocated blocks were found, then return 0
    if(!total_pl) {
        return 0.0;
    }
    // return ratio
    return (double) total_pl / total_size;
}
// Find maximum payload size, and then divide that by total heap size
// If heap size is 0, then return 0
double sf_utilization() {
    if(HEAP_SIZE() == 0) return 0.0;

    size_t heap_size = HEAP_SIZE();
    return (double) max_pl / heap_size;    
}
/**
 * @brief Helper function which creates an alloacted block from a given free block
 * @note free block should already be removed from the corresponding free list
 * @param free_block, pointer to sf_block that will be used for the allocated block
 * @param pl_size, payload size of allocated block
 * @returns *pp, pointer to the payload (NOT THE HEADER) of the newly allocated block
 */
void *create_malloc_block(sf_block* free_block, size_t pl_size) {
    // Grab free_block block_size
    size_t block_size = GET_BLOCK_SIZE(OBF(free_block -> header));

    // Grab footer of free block
    sf_footer *fPtr = FOOTER(free_block);

    // Pack header with corresponding values
    free_block -> header = OBF(PACK(pl_size, block_size, 0, 1));
    // Pack footer with same values
    *fPtr = free_block -> header;

    // Return pointer to payload
    return (void *) ((char*)free_block + MROW);
}
/**
 * @brief Validates the pointer passed in. The pointer should point to the payload, not the header
 * @returns 0 if valid, -1 if not
 * @param pp, pointer to payload
 */
int validate_pp(void *pp) {
    // First, verify that the pointer is from a malloc call
    // Is pointer null? Invalid
    if(!pp) return -1;

    // Is it not 16 byte-aligned? Invalid
    // Way to check: make sure starting address is multiple of 16
    if((size_t)pp % 16 != 0) return  -1; 

    // Now grab header and unobfuscate to compare
    sf_header * hPtr = (sf_header*) ((char*) pp - MROW);
    // XOR
    sf_header header = (sf_header)OBF(*hPtr);
    // grab block size
    size_t block_size = GET_BLOCK_SIZE(header);
    
    // is it less than 32 block size or not a multiple of 16, then invalid
    if(block_size < MIN_BLOCK_SIZE || block_size % 16 != 0) return -1; 
    // is the header before the start of the heap
    if((size_t)hPtr < (size_t)sf_mem_start()) return -1;

    // grab footer
    sf_footer* fPtr = (sf_footer *)((char*) header + block_size - MROW);
    // is footer after the end of the heap
    if((size_t)fPtr > (size_t)sf_mem_end()) return -1;

    // grab alloc bit
    int alloc = header & THIS_BLOCK_ALLOCATED;
    // grab quick list bit
    int in_ql = header & IN_QUICK_LIST;
    // if either alloc is 0 or in_ql is 1, abort
    if(!alloc || in_ql) return -1;
    
    // Else return 0
    return 0;
}
/**
 * @brief Splits passed in free block based on the blocksize passed in. Meant to be used for allocating blocks
 * @param free_block, pointer to the free block to split
 * @param block_size, block_size to allocate
 * @returns A new split (or not split) free block
 * @invariant free_block should not be present in the main list. 
 * And it will always be a main list free block, not a quick list one
 */
sf_block* split_free_block(sf_block* free_block, size_t block_size) {
    size_t fb_size = GET_BLOCK_SIZE(OBF(free_block -> header));
    size_t frag_size = fb_size - block_size;
    // Check if a fragment would be made, in which case free_block is returned as is
    if(frag_size < MIN_BLOCK_SIZE) {
        return free_block;
    }
    
    // Otherwise, continue splitting
    // Add new header information to beginning of block
    free_block -> header = OBF(PACK(0, block_size, 0, 0));
    // Add footer information
    sf_footer *footer = (sf_footer *) ((char *)free_block + block_size - MROW);
    *footer = free_block -> header; 

    // Grab pointer to where the next block (split one) should be stored
    char *nxtPtr = (char *) free_block + block_size;
    // Create free block starting at that address
    sf_block *fragment = create_free_block(frag_size, nxtPtr);

    // Insert fragment into main list, no point inserting into quick list since that will
    // most likely be popped from soon. 
    insert_ml(fragment);

    // Then return the newly split block
    return free_block;
}
/**
 * @brief Very similar to split_free_block, but this takes in an allocated block and splits it as needed.
 * Intended for sf_realloc() to call. The only difference between this and split_free_block is that this one coalesces.
 * @param block, pointer to block header (NOT PAYLOAD)
 * @param block_size, size to split into
 * @param pl_size, payload size
 * @returns block, pointer to the header of the split block
 */
sf_block* split_malloc_block(sf_block* block, size_t block_size, size_t pl_size) {
    size_t b_size = GET_BLOCK_SIZE(OBF(block -> header));
    size_t frag_size = b_size - block_size;
    // Check if a fragment would be made, in which case free_block is returned as is
    if(frag_size < MIN_BLOCK_SIZE) {
        return block;
    }
    
    // Otherwise, continue splitting
    // Add new header information to beginning of block
    block -> header = OBF(PACK(pl_size, block_size, 0, 1));
    // Add footer information
    sf_footer *footer = (sf_footer *) ((char *)block + block_size - MROW);
    *footer = block -> header; 

    // Grab pointer to where the next block (split one) should be stored
    char *nxtPtr = (char *) block + block_size;
    // Create free block starting at that address
    sf_block *fragment = create_free_block(frag_size, nxtPtr);

    // Coalesce as needed
    coalesce(fragment);

    // Insert fragment into main list, no point inserting into quick list since that will
    // most likely be popped from soon. 
    insert_ml(fragment);

    // Then return the newly split block
    return block;
}

/**
 * @brief: Simple helper function to abstract "unlinking" code. 
 * Unlinks the block passed in from its "prev" and "next" free blocks
 * @param block: pointer to block to unlilnk
 */
void unlink_block(sf_block *block) {
    sf_block *prev = block -> body.links.prev;
    sf_block *next = block -> body.links.next;
    
    block -> body.links.next = NULL;
    block -> body.links.prev = NULL;

    prev -> body.links.next = next;
    next -> body.links.prev = prev;
}
/**
 * @brief: Finds a block for the given block size
 * @param block_size: size to find corresponding block for
 * @returns block of free memory if found, NULL if not
 */
sf_block* find_fit(size_t block_size) {
    // printf("finding fit of size: %zu", block_size);
    // Get index of ML to search first
    int index = get_ml_index(block_size);
    // printf("Index: %d", index);
    // variable to store size of current block (used for comparing with size parameter)
    size_t curSize;
   
   // Iterate through each free list to find one
   for (int i = index; i < NUM_FREE_LISTS; i++) {
        // grab head of list
        sf_block *sentinel = sf_free_list_heads + i;
        sf_block *cur = sentinel;

        // Iterate through until a large enough block is found
        do {
            cur = cur -> body.links.next;
            // Grab current block size
            curSize = GET_BLOCK_SIZE(OBF(cur -> header)); 
        } while(cur != sentinel && curSize < block_size);

        // printf("out of while loop with cursize: %zu\n", curSize);
        if(cur != sentinel && curSize >= block_size) {
            // Return cur
            // printf("in cursize >= block_size with index: %d\n", i);
            return cur;
        } 
   }

   // Else, no valid block was found, return NULL
   return NULL;
}
/**
 * @brief Pops a block from the QL index
 * @param index, index of QL ot pop from
 */
sf_block* popQL(int index) {
    // Grab first pointer thing
    sf_block *first = sf_quick_lists[index].first;
    // Error checking
    if(!first) return NULL;

    // "Pop" first from list
    sf_quick_lists[index].first = first -> body.links.next;
    // decrease length
    sf_quick_lists[index].length--; 

    // Unlink it
    first -> body.links.next = NULL;
    // Return first
    return first;        
}

/**
 * @brief Initializes heap with the initial prologue value.
* @returns 0 on success, -1 on failure
*/
int initialize_heap() {
    // Grow heap, handling error
    char *ret = sf_mem_grow();   
    if (!ret) {
        sf_errno = ENOMEM;
        return -1;
    }
    
    // Initialize with prologue and epilogue
    // offset by one memory row, since first memory row is unused
    sf_block *prologue = (sf_block *)(sf_mem_start() + MROW);
    // Initialize with payload size  (0), block size (4  * MROW), 0 for QL alloc bit, and 1 for alloc bit
    sf_header prologue_header = OBF(PACK(0, 4 * MROW, 0, 1)); 
    
    // Set header of prologue block
    prologue -> header = prologue_header;
    // Set footer, which is just duplicate of header
    // Note for self: (char*) cast is necessary so that pointer arithmetic is correct (I want it in bytes)
    sf_footer *footer = FOOTER(prologue);
    // Replicate header to footer
    *footer = prologue -> header;

    // Initialize first free block
    // Subtract epilogue and prologue size from the total heap (along with the unused memory row at the beginning
    size_t block_size = 4096 - EPILOGUE_SIZE - PROLOGUE_SIZE - MROW;
    // 2 * MROW represents total size of header and footer
    sf_block *free_block = (sf_block*)create_free_block(block_size, ((char*) prologue + 4 * MROW));

    // Initialize epilogue, this should just be a header with only the allocation bit set (hopefully)
    sf_header *epilogue = (sf_header *)NEXT_BLOCK(free_block);
    *epilogue = OBF(0x0000000000000001);

    // Insert free block into the main list
    insert_ml(free_block);
    return 0; // Return 0 on completion
}

/**
 * @brief extend the heap and coalesce the block if needed
 * @returns 0 on success, -1 on failure
 */
int extend_heap() {
    // Grow heap, handling error
    char *ret = sf_mem_grow();   
    if (!ret) {
        sf_errno = ENOMEM;
        return -1;
    }  

    // Initialize the free block header & footer, this will start where the previous epilogue was 
    sf_block *free_block = create_free_block(PAGE_SZ, (ret - MROW));

    // Set epilogue
    // Grab last mem address offset by one row to insert epilogue
    char *end = (char *)sf_mem_end() - MROW;
    sf_header *epilogue = (sf_header*) (end);
    *epilogue = OBF(0x0000000000000001);

    // Coalesce free_block
    free_block = coalesce(free_block);
    // Insert coalesced block into main list
    insert_ml(free_block);
    
    return 0;
}

/* 
 * @brief Return index of corresponding free list based on size
 * Note: fl = free list
**/
int get_ml_index(size_t size) {
    size_t m = 32; // Minimum size

    for(int i = 0; i < NUM_FREE_LISTS-1; i++) {
        if(size <= m) return i;
        m =  m << 1; // Bitwise left by 1 (to multiply by 2)
    }

    return NUM_FREE_LISTS - 1;
}

/**
* @brief Creates a free block of the given size, starting at the given address
* @param block_size, size of the free block, including header, footer, & padding !IMPORTANT
* @param start_addr, starting address of the free block
* */
void *create_free_block(size_t block_size, char *start_addr) {
    sf_block *free_block = (sf_block*) start_addr;

    // Initialize header of free block
    free_block -> header = OBF(PACK(0, block_size, 0, 0));

    // Initialize footer of free block
    sf_footer* footer = (sf_footer*) ((char *) free_block + block_size - MROW);
    *footer = free_block -> header;
    return (void *)free_block;
}
/**
 * @brief Initialize both the main and quick free lists
 */
void initialize_free_lists() {
    // First, go through quick list and initialize
    // Note: the "first" free block will be NULL, so check this when inserting into the quick list 
    for(int i = 0; i < NUM_QUICK_LISTS; i++) {
        sf_quick_lists[i].length = 0;
    }

    // Then, go through main list and initialize
    for (int i = 0; i < NUM_FREE_LISTS; i++) {
        sf_block *cur = &sf_free_list_heads[i];
        // Initialize next and prev values  
        cur -> body.links.next = cur;
        cur -> body.links.prev = cur;
        // cur -> header = OBF((size_t)0);
    }
}
/**
* @brief Inserts the free block into the corresponding main list
* @param free_block, pointer to the free block
*/
void insert_ml(sf_block* free_block) {
    /// Grab block size
    size_t block_size = GET_BLOCK_SIZE(OBF(free_block -> header));

    //Grab index
    int index = get_ml_index(block_size);
    // Grab sentinel of respective list
    sf_block *sentinel = (sf_free_list_heads + index);
    
    // Insert into list
    sf_block *next = sentinel -> body.links.next;
    sentinel -> body.links.next = free_block;
    next ->  body.links.prev = free_block;
    free_block -> body.links.prev = sentinel;
    free_block -> body.links.next = next;

}

/**
 * @brief Removes the given free_block from the corresponding main free list
 * @param free_block, address of the free_block to remove
 * @returns 0 on success, -1 on failure (if the free_block is not found in the list)
 * @note, this function is kinda obselete since I can just call unlink_block() which is much more efficient. 
 * However, this function also tells me if a given block passed in can't be found in any free list. 
 * But I'm not sure that's an edge case I really need to check as every free block should be in a list
 * Something to just keep in mind.
 */
int remove_ml(sf_block *free_block) {
    // Grab block size
    size_t block_size = GET_BLOCK_SIZE(OBF(free_block -> header));
    // Grab index
    int index = get_ml_index(block_size);
    // Grab sentinel
    sf_block *sentinel = (sf_free_list_heads + index);

    // Iterate until the free_block is found
    sf_block *cur = sentinel;
    do {
        cur = cur -> body.links.next; 
    } while (cur != free_block && cur != sentinel);

    // free_block wasn't found in any list
    if(cur == sentinel) return -1;

    // Else, cur = free_block, so remove/unlink it
    unlink_block(cur);

    // Return 0 on success
    return 0;
}
/**
 * @brief Insert a free block into the corresponding quick list
 * @param free_block, pointer to the free block sf_block struct
*/
void insert_ql(sf_block * free_block) {
    // Store unobfuscated header
    sf_header header = OBF(free_block -> header);
    // Grab block size
    size_t block_size = GET_BLOCK_SIZE(header);

    // Grab index
    int index = QL_INDEX(block_size);

    // Grab list struct
    int length = sf_quick_lists[index].length;
    if(length == 5) {
        // List should be flushed here (all the pointers should be coalesced and inserted back into the main list)
        flush_ql(index);
    }
    // Can be added to list otherwise
    // Set QL and alloc bit of header
    free_block -> header = OBF(header | IN_QUICK_LIST | THIS_BLOCK_ALLOCATED);

    // Insert into list
    sf_block *prev_first = sf_quick_lists[index].first;
    sf_quick_lists[index].first = free_block;
    free_block -> body.links.next = prev_first;
    
    //Increment length
    sf_quick_lists[index].length++;
}

/**
 * @brief Will flush the quick list of the given head (insert each QL into the respective main list)
 * @param index, index of the given QL to flush 
 */
void flush_ql(int index) {
    if(index > NUM_QUICK_LISTS) {
        fprintf(stderr, "Invalid QL index");
        return;
    }

    // Grab "first" pointer
    sf_block *cur = sf_quick_lists[index].first;
    while(cur != NULL) {
        // Set QL first to the next one
        sf_quick_lists[index].first = cur -> body.links.next;

        // Unlink cur
        cur -> body.links.next = NULL;
        // Coalesce cur
        cur = coalesce(cur);
        // Insert cur into main list
        insert_ml(cur);

        // Set cur back to the QL first
        cur = sf_quick_lists[index].first;
    } 
}


/**
 * @brief Will coalesce the given free block with the preceding and succeeding blocks (if they are also free)
 * @param free_block, pointer to the free block to coalesce
 * @note Insertion into the main list will be handled outside of this function. As in, I'll call coalesce, and then insert the returned block. 
 * I think this is better personally as I feel like insertion shouldn't be abstracted away into this 
 * function or else it leads to the code being weird/unreadable I guess
 */
sf_block *coalesce(sf_block *free_block) {
    // Grab prev block and prevHdr
    sf_block *prev = PREV_BLOCK(free_block);
    sf_header prevHdr = OBF(prev -> header);

    // Grab block size
    size_t block_size = GET_BLOCK_SIZE(OBF(free_block -> header));

    // Grab next block and nextHdr
    sf_block* next = NEXT_BLOCK(free_block);
    sf_header nextHdr = OBF(next -> header);

    // Grab each header/footer's allocation bit
    int nextAlloc = nextHdr & THIS_BLOCK_ALLOCATED;
    int prevAlloc = prevHdr & THIS_BLOCK_ALLOCATED;
    
    // Case 1: both prev and next are allocated
    if(prevAlloc && nextAlloc) return free_block;

    // Case 2: next block is free
    else if(prevAlloc && !nextAlloc) {
        // Inrement block size
        block_size += GET_BLOCK_SIZE(nextHdr);

        // Remove next block from it's corresponding list, it can only be in a main list because a quick list block wouldn't be free
        int ret = remove_ml(next); 
        if(ret) fprintf(stderr, "Failed to find next block to remove while coalescing");

        // Pack header with new size
        free_block -> header = OBF(PACK(0, block_size, 0, 0));        
        // Pack footer with new size
        sf_footer * footer = FOOTER(free_block);
        *footer = free_block -> header;
        return free_block;
    }

    // Case 3: prev block is free
    else if(!prevAlloc && nextAlloc) {
        // Increment block size
        block_size += GET_BLOCK_SIZE(prevHdr);
        
        // Remove prev from it's old list
        int ret = remove_ml(prev);
        if(ret) fprintf(stderr, "Failed to find prev block to remove while coalescing");

        // Pack header with new size
        prev -> header = OBF(PACK(0, block_size, 0, 0));
        // Pack footer with new size
        sf_footer * footer = FOOTER(prev);
        *footer = prev -> header;
        return prev;
    }

    // Case 4: prev and next block are both free
    else {
        // Increment block size
        // printf("in else where both prev and next are free");
        block_size += GET_BLOCK_SIZE(prevHdr) + GET_BLOCK_SIZE(nextHdr);
        
        // Remove next block from it's corresponding list, it can only be in a main list because a quick list block wouldn't be free
        int ret = remove_ml(next); 
        if(ret) fprintf(stderr, "Failed to find next block to remove while coalescing");
        // Remove prev from it's old list
        ret = remove_ml(prev);
        if(ret) fprintf(stderr, "Failed to find prev block to remove while coalescing");

        // Pack header with new size
        prev -> header = OBF(PACK(0, block_size, 0, 0));
        // Pack footer with new size
        sf_footer *footer = FOOTER(prev);
        *footer = prev -> header;
        return prev;
    }
}