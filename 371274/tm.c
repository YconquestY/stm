/**
 * @file   tm.c
 * @author Yue Yu <yue.yu@epfl.ch>
 *
 * @section LICENSE
 * 
 * Copyright © 2023 Yue Yu.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version. Please see https://gnu.org/licenses/gpl.html
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * @section DESCRIPTION
 *
 * Dual-versioned software transactional memory (DV-STM) implementation.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
//#include <stdint.h>
//#include <stdbool.h>
//#include <string.h>
//#include <stdatomic.h>
//#include <immintrin.h> // SIMD intrinsics

// Internal headers
#include <tm.h>

#include "macros.h"
#include "batcher.h"

/**
 * @brief Allocate a segment
 * 
 * @param shared Shared memory region to allocate a segment in
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param first  Whether this is the first segment
 * @return Opaque pointer to first word of allocated segment
 *             0x1000 0000…0000 on failur
 *             0x0100 0000…0000 if too many segments
**/
shared_t alloc_segment(shared_t shared, size_t size, bool first)
{
    struct region* region = (struct region*) shared;
    // Get segment ID
    uint8_t seg_id;
    acquire(&(region->top_lock));
    if (first) { // Non-free-able first segment
        seg_id = FIRST_SEG; region->top = FIRST_SEG + 1;
        release(&(region->top_lock));
    }
    else if (unlikely(region->top >= MAX_SEG)) { // Too many segments
        release(&(region->top_lock));
        return SEG_OVERFLOW;
    }
    else {
        seg_id = region->segment_id[region->top++];
        release(&(region->top_lock));
    }
    size_t _align = first ? region->align : region->_align; // Alignment actually used
    // Compute sizes
    size_t num_words = size / align;
    size_t metad_size = sizeof(struct segment_node)
                      + num_words * sizeof(atomic_flag) // Per-word "access set" guard
                      + num_words * sizeof(uint64_t);   // Per-word "access set" and written? flag
    // Allocate memory
    struct segment_node* sn;
    if (unlikely(posix_memalign((void**) &sn, _align,
                                metad_size + 2 * size) != 0)) { // Allocation failed
        return NOMEM;
    }
    region->allocs[seg_id] = sn; // Register segment in region
    // Initialize control structures
    sn->seg_id = seg_id;
    sn->size   = size;
    
    sn->freed   = ATOMIC_FLAG_INIT;
    sn->written = ATOMIC_FLAG_INIT;

    sn->aset_locks = (atomic_flag*) (sn + sizeof(struct segment_node));
    for (size_t i = 0; i < num_words; i++) {
        sn->aset_locks[i] = ATOMIC_FLAG_INIT;
    }
    sn->aset = (uint64_t*) (sn->aset_locks + num_words * sizeof(atomic_flag));
    memset(sn->aset, 0, num_words * sizeof(uint64_t));

    sn->version = 0;
    void* segment = (void*) ((uintptr_t) sn + metad_size);    
    sn->copies[0] = segment;
    sn->copies[1] = (void*) ((uintptr_t) segment + size);
    // Initialize segment memory
    memset(segment, 0, 2 * size);
    // Opaque address
    uintptr_t oaddr = (uintptr_t) seg_id;
    return (shared_t) (oaddr << SHIFT);
}

/**
 * @brief Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * 
 * No TX has access to the first non-free-able segment. Hence, it only takes a
 * single-versioned layout with no per-word control structure.
 * 
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
**/
shared_t tm_create(size_t size, size_t align) {
    struct region* region = (struct region*) malloc(sizeof(struct region));
    if (unlikely(!region)) {
        return invalid_shared;
    }
    // Initialize batcher
    if (unlikely(!batcher_init(&(region->batcher)))) {
        free(region);
        return invalid_shared;
    }
    // Segment ID stack; must initialize before allocating first segment
    region->top_lock = ATOMIC_FLAG_INIT;
    region->top = FIRST_SEG; // Segment ID starts from 1.
    for (size_t i = 0; i < MAX_SEG; i++) {
        region->segment_id[i] = i;
    }
    // Determine alignment; must initialize before allocating first segment
    region->align  = align;
    region->_align = align < sizeof(struct segment_node*) ? sizeof(void*) : align; // max{user-defined alignment, pointer size}

    memset(region->allocs, 0, MAX_SEG * sizeof(struct segment_node*)); // Initialize segment list
    // Allocate first segment; assume no failure
    // TODO: region partially initialized?
    shared_t first = alloc_segment((shared_t) region, size, true);
    if (unlikely((first == NOMEM) || (first == SEG_OVERFLOW))) { // Allocation failed
        batcher_cleanup(&(region->batcher)); free(region);
        return invalid_shared;
    }
    // Success: initializa region
    region->start  = first;
    region->size   = size;
    
    memset(region->history, 0, MAX_RW_TX * sizeof(struct record*));
    //memset(region->write, 0, MAX_RW_TX * sizeof(bool));
    //memset(region->whistory, 0, MAX_RW_TX * sizeof(struct wrecord*));
    //memset(region->mark, 0, MAX_RW_TX * sizeof(bool));
    //memset(region->fhistory, 0, MAX_RW_TX * sizeof(struct frecord*));

    return (shared_t) region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t shared) {
    struct region* region = (struct region*) shared;
    // Clean up batcher
    batcher_cleanup(&(region->batcher));
    // Destroy all segments
    for (uint8_t i = FIRST_SEG; i < MAX_SEG; i++) {
        if (region->allocs[i]) { // Segment exists
            free(region->allocs[i]); // Do not free internal pointers
        }
    }
    clear_history(shared); // Clear up all TXs' op history
    //clear_whistory(shared); // Clear up write history
    //clear_fhistory(shared); // Clear up free history
    free(region); // Clear up entire region
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t shared) {
    return ((struct region*) shared)->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t shared) {
    return ((struct region*) shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t shared) {
    return ((struct region*) shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t shared, bool is_ro) {
    return batcher_enter(&( ((struct region*) shared)->batcher ), is_ro);
}

/**
 * @brief [thread-safe] End the given transaction.
 * 
 * A TX successfully commits by calling `tm_end`.
 * 
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t shared, tx_t tx) {
    batcher_leave(shared, tx, true); // Leave batch
    // Word swap deferred until all TXs leave current batch
    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    // Prepare translating opaque source address to virtual address
    uint8_t seg_id = (uint8_t) ((uintptr_t) source >> SHIFT); // Segment ID
    size_t offset = (size_t) ((uintptr_t) source & ADDR_OFFSET); // Opaque address; multiple of `align`

    struct region* region = (struct region*) shared;
    struct segment_node* sn = region->allocs[seg_id]; // Segment node
    int version = sn->version; // RO version
    // RO TX
    if (tx >= MAX_RW_TX) {
        void* vaddr = (void*) ((uintptr_t) (sn->copies[version]) + offset); // Virtual address
        memcpy(target, vaddr, size);
        return true;
    }
    // R/W TX
    size_t word_idx = offset / region->align; // Starting word index
    size_t num_words = size / region->align;  // No. of words to read
    // Check whether to abort
    uint64_t pattern = (uint64_t) 1 << tx;
    for (size_t i = word_idx; i < word_idx + num_words; i++)
    {   // Acquire per-word "access set" lock
        acquire(&(sn->aset_locks[i]));
        
        uint64_t bitmap = sn->aset[i];
        if (!(  (bitmap == (WRITTEN | pattern)) // Word written by current TX
             || (bitmap < WRITTEN)))            // Word not written
        {   // Release per-word lock
            for (size_t j = word_idx; j <= i; j++) {
                release(&(sn->aset_locks[j]));
            }
            batcher_leave(shared, tx, false); // Leave batch
            return false; // Abort TX
        }
    }
    // Configure "access sets"
    // TODO: "access set" update optimization
    for (size_t i = word_idx; i < word_idx + num_words; i++) {
        sn->aset[i] |= pattern;
    }
    // Read words
    void* vaddr = (void*) ((uintptr_t) (sn->copies[1 - version]) + offset); // Virtual address
    memcpy(target, vaddr, size);
    // Release per-word "access set" lock
    for (size_t i = word_idx; i < word_idx + num_words; i++) {
        release(&(sn->aset_locks[i]));
    }
    // Update TX history
    struct record* r = rw(READ, seg_id, offset, size, region->_align);
    r->next = region->history[tx];
    region->history[tx] = r;

    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t shared, tx_t tx, void const* source, size_t size, void* target) {
    // Prepare translating opaque target address to virtual address
    uint8_t seg_id = (uint8_t) ((uintptr_t) target >> SHIFT); // Segment ID
    size_t offset = (size_t) ((uintptr_t) target & ADDR_OFFSET); // Opaque address; multiple of `align`

    struct region* region = (struct region*) shared;
    struct segment_node* sn = region->allocs[seg_id]; // Segment node
    int version = sn->version; // RO version

    size_t word_idx = offset / region->align; // Starting word index
    size_t num_words = size / region->align;  // No. of words to write
    // Check whether to abort
    uint64_t pattern = (uint64_t) 1 << tx;
    for (size_t i = word_idx; i < word_idx + num_words; i++)
    {   // Acquire per-word "access set" lock
        acquire(&(sn->aset_locks[i]));

        uint64_t bitmap = sn->aset[i];
        if (bitmap & ~WRITTEN & ~pattern > 0) // Word read/written by other TX
        {   // Release per-word lock
            for (size_t j = word_idx; j <= i; j++) {
                release(&(sn->aset_locks[j]));
            }
            batcher_leave(shared, tx, false); // Leave batch
            return false; // Abort TX
        }
    }
    // Configure "access sets"
    // TODO: "access set" update optimization
    for (size_t i = word_idx; i < word_idx + num_words; i++) {
        sn->aset[i] |= WRITTEN | pattern;
    }
    // Write words
    void* vaddr = (void*) ((uintptr_t) (sn->copies[1 - version]) + offset); // Virtual address
    memcpy(vaddr, source, size);
    // Release per-word "access set" lock
    for (size_t i = word_idx; i < word_idx + num_words; i++) {
        release(&(sn->aset_locks[i]));
    }
    // Update TX history
    struct record* r = rw(WRITE, seg_id, offset, size, region->_align);
    r->next = region->history[tx];
    region->history[tx] = r;
    //region->write[tx] = true;

    //struct wrecord* wr;
    //posix_memalign((void**) &wr, region->_align, sizeof(struct wrecord)); // Assume no failure due to small size
    //wr->seg_id = seg_id;
    //wr->offset = offset;
    //wr->size   = size;
    // Insert as history head
    //wr->next = region->whistory[tx];
    //region->whistory[tx] = wr;
    return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared, tx_t tx, size_t size, void** target) {
    // Allocate segment
    shared_t oaddr = alloc_segment(shared, size, false);
    // I did not use a `switch` block for the sake of branch prediction hints.
    // Not enough memory
    if (unlikely(oaddr == NOMEM)) {
        batcher_leave(shared, tx, false); // Leave batch
        return nomem_alloc;               // Abort TX
    }
    // Too many segments
    else if (unlikely(oaddr == SEG_OVERFLOW)) {
        batcher_leave(shared, tx, false); // Leave batch
        return abort_alloc;               // Abort TX
    }
    // Success: segment already registered in region
    struct region* region = (struct region*) shared;
    // Update TX history
    struct record* r = af(ALLOC, (uint8_t) (oaddr >> SHIFT), region->_align);
    r->next = region->history[tx];
    region->history[tx] = r;

    *target = oaddr;
    return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared, tx_t tx, void* target) {
    uint8_t seg_id = (uint8_t) ((uintptr_t) target >> SHIFT); // Segment ID
    if (unlikely(seg_id == FIRST_SEG)) { // Deallocate first segment
        batcher_leave(shared, tx, false); // Leave batch
        return false; // Cannot free first segment, abort TX
    }
    // TODO: update comment
    // Mark segment for deregistration by updating free history
    // If the calling TX aborts, the segment will not be freed.
    struct region* region = (struct region*) shared;

    struct record* r = af(FREE, seg_id, region->_align);
    r->next = region->history[tx];
    region->history[tx] = r;
    //region->mark[tx] = true;
    
    //struct frecord* fr;
    //posix_memalign((void**) &fr, region->_align, sizeof(struct frecord)); // Assume no failure due to small size
    //fr->seg_id = seg_id;
    // Insert as history head
    //fr->next = region->fhistory[tx];
    //region->fhistory[tx] = fr;

    return true;
}
