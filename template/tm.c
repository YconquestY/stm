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
 * Dual-versioned software transactional memory implementation.
**/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
//#include <stdint.h>  // Already in `tm.h`
//#include <stdbool.h> // Already in `tm.h`
#include <string.h>
//#include <stdatomic.h>
//#include <immintrin.h> // SIMD intrinsics

// Internal headers
#include <tm.h>

#include "macros.h"
#include "batcher.h"
#include "utils.h"   // Atomic variable as spinlock

#define BITS_PER_CHAR 8

/**
 * @brief Allocate a segment
 * 
 * @param size  Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes), must be a power of 2
 * @param first Whether this is the first segment
 * @return Pointer to the allocated segment (not the first word); `NULL` on failure
**/
struct segment_node* alloc_segment(size_t size, size_t align, bool first)
{   // Determine alignment
    size_t _align; // Alignment actually used
    if (first) {
        _align = align;
    }
    // Later segments: max{`align`, `sizeof(struct segment_node*)`}
    else if (align < sizeof(struct segment_node*)) {
        _align = sizeof(void*);
    }
    else {
        _align = align;
    }
    // Compute sizes
    size_t num_words = size / align;
    size_t num_wbitmap = (num_words + BITS_PER_CHAR - 1) / BITS_PER_CHAR; // Wrong: `num_words / 8 + 1`
    size_t metad_size = sizeof(struct segment_node)
                      + num_wbitmap * sizeof(unsigned char) // Written? bitmap every 8 words
                      + num_words * sizeof(atomic_flag)     // Per-word "access set" guard
                      + num_words * sizeof(uint64_t);       // Per-word "access set"
    // Allocate memory
    struct segment_node* sn;
    if (unlikely(posix_memalign((void**) &sn, _align,
                                metad_size + 2 * size) != 0)) { // Allocation failed
        return NULL;
    }
    // Initialize control structures
    sn->first = first;
    sn->marked  = ATOMIC_FLAG_INIT;
    sn->freed   = ATOMIC_FLAG_INIT;
    sn->written = ATOMIC_FLAG_INIT;

    sn->written = (unsigned char*) (sn + sizeof(struct segment_node));
    memset(sn->written, 0, num_wbitmap * sizeof(unsigned char));

    sn->aset_locks = (atomic_flag*) (sn->written + num_wbitmap * sizeof(unsigned char));
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

    sn->prev = NULL;
    
    return sn;
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
    // Allocate first segment
    region->start = (shared_t) alloc_segment(size, align, true);
    if (unlikely(!(region->start))) {
        free(region); batcher_cleanup(&(region->batcher));
        return invalid_shared;
    }
    region->start->next = NULL; // No next segment
    // Initialize region
    region->insert = ATOMIC_FLAG_INIT;
    region->allocs = NULL;
    region->size   = size;
    region->align  = align;
    memset(region->write, 0, MAX_RW_TX * sizeof(bool));
    memset(region->whistory, 0, MAX_RW_TX * sizeof(wrecord*));

    return region;
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
**/
void tm_destroy(shared_t unused(shared)) {
    // TODO: tm_destroy(shared_t)
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
**/
void* tm_start(shared_t unused(shared)) {
    return ((struct region*) shared)->start;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
**/
size_t tm_size(shared_t unused(shared)) {
    return ((struct region*) shared)->size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
**/
size_t tm_align(shared_t unused(shared)) {
    return ((struct region*) shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
**/
tx_t tm_begin(shared_t unused(shared), bool unused(is_ro)) {
    tx_t tx_id = batcher_enter(&( ((struct region*) shared)->batcher), is_ro);
    if (unlikely(tx_id == -1)) {
        return invalid_tx;
    }
    else {
        return tx_id;
    }
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
**/
bool tm_end(shared_t unused(shared), tx_t unused(tx)) {
    // TODO: how to notify region of segment deregistration?
    //atomic_flag_test_and_set(&( ((struct region*) shared)->dereg )); // Notify region of deregistration
    return false;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
**/
bool tm_read(shared_t unused(shared), tx_t unused(tx), void const* unused(source), size_t unused(size), void* unused(target)) {
    // TODO: tm_read(shared_t, tx_t, void const*, size_t, void*)
    return false;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
**/
bool tm_write(shared_t unused(shared), tx_t unused(tx), void const* unused(source), size_t unused(size), void* unused(target)) {
    // TODO: tm_write(shared_t, tx_t, void const*, size_t, void*)
    return false;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
**/
alloc_t tm_alloc(shared_t shared, tx_t unused(tx), size_t size, void** target) {
    // Allocate segment
    struct segment_node* sn = alloc_segment(size, ((struct region*) shared)->align, false);
    if (unlikely(!sn)) {
        return nomem_alloc;
    }
    // Insert as head of linked list: data race!
    acquire(&( ((struct region*) shared)->insert ));
    sn->next = ((struct region*) shared)->allocs;
    if (sn->next) { // Nonempty memory region
        sn->next->prev = sn;
    }
    ((struct region*) shared)->allocs = sn;
    release(&( ((struct region*) shared)->insert ));

    *target = sn->copies[0];
    return success_alloc;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
**/
bool tm_free(shared_t shared, tx_t unused(tx), void* target) {
    struct segment_node* sn = (struct segment_node*) ((uintptr_t) target - sizeof(struct segment_node));
    sn->tofree = true; // Mark segment for deregistration
    // Wait for end of epoch

    return true; // Always TRUE?
}
