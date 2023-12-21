/**
 * @file   batcher.h
 * @author Yue Yu (yue.yu@epfl.ch)
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
 * Although named `batcher.h`, this file also include utilities other than the
 * thread batcher.
 * 
 * Table of contents
 *     0. Constants and DV-STM components
 *     1. Thread batcher utilities
 *     2. Use `atomic_flag` as lock
 *     3. TX operation history utilities
**/
#pragma once

// Requested features
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE   200809L
#ifdef __STDC_NO_ATOMICS__
    #error Current C11 compiler does not support atomic operations
#endif

// External headers
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

// Internal headers
#include <tm.h>

#include "macros.h"

/*********************************
 * 0. Constants and declarations *
 *********************************/

// Max no. of R/W TX per batch
// The fundamental reason of confining the max. no. of R/W TX per batch is the
// per-word "access set". The written? flag and an "access set" is fused into
// an `uint64_t`. The MSB signals if a word is written, and the remaining 63b
// act as a bitmap, each representing a R/W TX. Extra R/W TX will be rejected
// when calling `tm_begin`.
#define MAX_RW_TX 63
// Max no. of segments per region (actually 63 because 0th slot unused)
#define MAX_SEG   64
#define FIRST_SEG 1

#define SHIFT        48
#define NOMEM        0x1000000000000000 // Only first hex digit set
#define SEG_OVERFLOW 0x0100000000000000 // Only second hex digit set
#define ADDR_OFFSET  0x0000FFFFFFFFFFFF // Least 48b set
#define WRITTEN      0x8000000000000000 // MSB set

/**
 * @brief Thread batcher.
 */
struct batcher_t {
    uint64_t counter; // Current epoch
    tx_t rw_tx; // R/W TX ID from 0 to `MAX_RW_TX` - 1; `invalid_tx`: extra R/W TX rejected
    tx_t ro_tx; // RO  TX ID from `MAX_RW_TX`; no no. limit
    uint64_t remaining; // No. of outstanding threads in current epoch
    uint64_t blocked;   // No. of waiting threads, to be executed in next epoch
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/**
 * @brief A (dynamically allocated) segment and its control metadata.
 * 
 * `segment_node` and `region` are moved here so that `batcher_leave` 
 * recognizes these constructs.
**/
struct segment_node
{   // Segment ID; no more than `MAX_SEG`
    uint8_t seg_id; // First segment has ID `FIRST_SEG`, i.e., 1; futile?
    size_t size;    // Segment size
    
    atomic_bool freed;   // Confirmed to be freed at epoch end
    atomic_bool written; // Confirmed to have been written at epoch end
    
    atomic_flag* aset_locks; // Per-word "access set" guard
    uint64_t* aset;          // Per-word "access set" and written? flag
    void* ro; // Read-only  copy
    void* rw; // Read/write copy
};
typedef struct segment_node* segment_list;

/**
 * @brief Op type per record (TX op).
**/
typedef
enum {READ, WRITE, ALLOC, FREE}
op_t;

/**
 * @brief `tm_read`/`tm_write` record.
**/
struct rwop {
    uint8_t seg_id;
    size_t  offset;
    size_t  size;
};

/**
 * @brief `tm_alloc`/`tm_free` record.
**/
struct afop {
    uint8_t seg_id;
};

/**
 * @brief A R/W TX op record.
 * 
 * TODO: how does `posix_memalign` behave for `struct` with `union`?
 */
struct record
{
    op_t type;
    struct record* next;
    union {
        struct rwop rwop; // `tm_read`  or `tm_write`
        struct afop afop; // `tm_alloc` or `tm_free`
    };
};

/**
 * @brief Shared memory region, a.k.a. transactional memory.
**/
struct region
{   // Thread batcher
    struct batcher_t batcher;
    // Non-free-able first segment
    shared_t start; // Pointer to first word of first segment
    size_t size;    // Size of first segment
    size_t align;   // Global alignment, i.e., size of a word
    // The no. of all segments (including the non-free-able one) is capped at
    // `MAX_SEG` (actually 63). The fundamental reason is that I want to deduce
    // which segment a generic TX accesses given an opaque `void*` pointer. A
    // generic pointer, a.k.a. `void*`, is 8B long. Any segment is no longer
    // than 2^48B, whose addresses are representable by 6B. Therefore, I
    // defined another abstraction of opaque address: a `void*` ranges
    //     from 0x#### 0000…0000
    //     to   0x#### FFFF…FFFF.
    // The highest 2B is wasted! Hence, I use second highest 1B (actually 6b)
    // to signal segment ID. E.g., addresses of the 5th segment (including the
    // non-free-able one) look like
    //     0x0005 ####…####,
    // and `void*` now ranges
    //     from 0x##01 ####…####
    //     to   0x##3F ####…####.
    //              ^^ segment ID
    // Note that a region supports a max. of 63 segments. 0x##00 ####…#### is
    // not used, and the non-free-able first segment starts from
    // 0x01 ####…####. This is because `tm_start` should not return `NULL`,
    // which is 0x##00 0000…0000 if the first segment is assigned ID 0. Segment
    // IDs starts from 1.
    atomic_flag top_lock; // Stack top guard
    // Segment stack top
    // `top` starts from `FIRST_SEG`, i.e., 1, as explained above. Besides,
    // `top` - 1 is the no. of IDs already assigned to segments. Note that
    // neither [1,`top`) nor `segment_id[1…top]` contains valid segment IDs.
    // Consider the case
    //                 create, alloc, alloc, alloc, free 3, free 2, alloc
    //     Assigned ID:     1      2      3      4                      2,
    // Valid segment IDs are 1, 2, and 4; `top` is 4; `segment_id` is
    //     {0, 1, 2, 2, 3, 5, …， 63}
    //                  ^ Stack top
    // Obviously, `segment_id` may not store consecutive IDs in that freed IDs
    // are pushed back atop.
    uint8_t top;
    uint8_t segment_id[MAX_SEG]; // Stack for segment IDs; `segment_id[1]` is stack top
    struct segment_node* allocs[MAX_SEG]; // All segments    
    // Per-TX op history
    // While RO TXs always commit, a R/W TX may abort, and any op of the TX
    // prior to the abort point must be rolled back. Hence, per-TX history is
    // necessary for op rollback. Now that no. of R/W TXs is capped at
    // `MAX_RW_TX`, the history is centrally managed per region. History of
    // each R/W TX is a linked list of records. Note that
    // 1. Rollback is absolutely necessary.
    //    One cannot simply ignore a TX's mess after aborting it. Consider the
    //    case
    //        TX | Trace
    //         I | W W … R W
    //        II |           R W
    //    Suppose all ops but the last write of TX I commits. TX I aborts. All
    //    previous writes must be rolled back. Otherwise,
    //    1.1 atomicity would be violated;
    //    1.2 TX II would abort, which could have committed.
    // 2. All R/W TX ops, including `tm_read(…)` and `tm_alloc(…)`, may be
    //    rolled back.
    // 3. Lazy execution seems infeasible.
    //    3.1 Writes are from temporary buffers in library user memory.
    //        Contents of the buffers may have changed when the R/W TX commits.
    //    3.2 Consider the case
    //            TX: W W … R W (assume same location)
    //        (Point 3 of snapshot isolation) TX has a read in the end. If
    //        the read targets a previously written word, writes cannot be
    //        deferred. This is because a TX must see its own modifcations.
    // 4. When rolling back writes, copy from RO to R/W versions. It is
    //    infeasible to "remember" initial contents of the R/W version.
    // 5. Do not write to both copies of memory words. There may be outstanding
    //    RO TXs after the R/W TX.
    // 6. If there are multiple accesses to the same word in a R/W TX, the
    //    history will contain repetitive records. Then the same word will be
    //    rolled back multiple times. Although inefficient, I do not optimize
    //    it because this is the uncommon case. It is insane for a library user
    //    to keep reading/writing the same exact word!
    //        "Make the common case fast; make the uncommon case correct."
    struct record* history[MAX_RW_TX];
};

/*********************
 * 1. Thread batcher *
 *********************/

/** Initialize the thread batcher; called by `tm_create`.
 * @param batcher Thread batcher to initialize
 * @return Whether the operation is a success
**/
bool batcher_init(struct batcher_t* batcher);

/** Clean the thread batcher up; called by `tm_destroy`.
 * @param batcher Thread batcher to clean up
**/
void batcher_cleanup(struct batcher_t* batcher);

/** Wait and enter a batch.
 * @param batcher Thread batch to enter
 * @param is_ro   Whether the TX is read-only
 * @return TX ID; `invalid_tx` if R/W TX no. exceeds `MAX_RW_TX`
**/
tx_t batcher_enter(struct batcher_t* batcher, bool is_ro);

/**
 * @brief Leave the current batch.
 * 
 * The interface is inconsistent with the reference implementation. First, the
 * opaque TX handle is a added. Meanwhile, there is a boolean indicating if the
 * TX successfully commits. This is because
 *     1. An aborted TX must roll back its previous writes.
 * `tm_end` is called only if all ops of a TX succeed. Aborted TXs do not call
 * `tm_end` but leave the current batch directory.
 * 
 * Second, the batcher handle is replaced with a TM handle. This is because not
 * only 1. but also
 *     2. Segment deregistration and word swap (a.k.a. snapshot installation)
 *        are deferred to the end of the current epoch.
 * Both tasks are centrally managed by a region, which means knowing the
 * batcher itself is not enough.
 * 
 * @param shared    Shared memory region to leave
 * @param batcher   Thread batch to leave
 * @param committed Whether the TX successfully commits
**/
void batcher_leave(shared_t shared, tx_t tx, bool committed);

/********************************
 * 2. Use `atomic_flag` as lock *
 ********************************/

/** Acquire spinlock.
 * @param lock Pointer to atomic flag
**/
/*static inline*/ void acquire(atomic_flag* lock);

/** Release spinlock.
 * @param lock Pointer to atomic flag
**/
/*static inline*/ void release(atomic_flag* lock);

/*************************************
 * 3. TX operation history utilities *
 *************************************/

/** R/W TX: insert a read/write record.
 * @param type   Type of operation
 * @param seg_id ID of segment read/written
 * @param offset Offset against segment start
 * @param size   Read/write size (in bytes)
 * @param align  Alignment of read/written memory (in bytes)
**/
struct record* rw(op_t type,
                  uint8_t seg_id, size_t offset, size_t size,
                  size_t align);

/** R/W TX: insert an alloc/free record.
 * @param type   Type of operation
 * @param seg_id ID of segment allocated/freed
 * @param align  Alignment of allocated memory (in bytes)
**/
struct record* af(op_t type, uint8_t seg_id, size_t align);

/** Clear up all TXs' op history.
 * @param shared Shared memory region to get history from
**/
void clear_history(shared_t shared);
