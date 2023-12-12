#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

// Max no. of R/W TX per batch
// The fundamental reason of confining the max. no. of R/W TX per batch is the
// per-word "access set". I use an `uint664_t` as a bitmap, each bit
// representing a R/W TX. Extra R/W TX will be rejected when calling `tm_begin`.
#define MAX_RW_TX 64

/**
 * @brief Thread batcher.
 */
struct batcher_t {
    int counter; // Current epoch ID
    int rw_tx;   // R/W TX ID from 0 to `MAX_RW_TX` - 1; -1: extra R/W TX rejected
    int ro_tx;   // RO  TX ID from `MAX_RW_TX`; no no. limit
    int remaining; // No. of unfinished threads in current epoch
    int blocked;   // No. of waiting threads, to be executed in next epoch
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/**
 * @brief List of dynamically allocated segments.
 * 
 * `segment_node` and `region` are moved here so that `batcher_leave` may
 * continue `tm_free`ing marked segments.
 * 
 * TODO: word swap optimization
**/
struct segment_node
{
    struct segment_node* prev;
    struct segment_node* next;
    bool first; // Whether the first segment
    // Whether to free segment at epoch end
    // Traversing the linked list of segments to free memory is not efficient.
    // However, it seems to be a must to traverse the linked list to swap word
    // copies when building a new snopshot at epoch end. Hence, segment
    // deregistration is trivially combined with snapshot construction.
    atomic_flag marked, freed; // For freeing segment
    atomic_flag written;

    unsigned char* written;  // Written? bitmap every 8 words
    atomic_flag* aset_locks; // Per-word "access set" guard
    uint64_t* aset;          // Per-word "access set"
    int version;     // 0        : A; 1        : B
    void* copies[2]; // copies[0]: A; copies[1]: B
    // word allocated at runtime
};
typedef struct segment_node* segment_list;

/**
 * @brief Write record for rollback.
 * 
 * A write history comprises multiple write records.
**/
struct wrecord {
    shared_t target; // Starting address of written word
    size_t size;     // Write size
    struct wrecord* next;
};

/**
 * @brief Shared memory region, a.k.a. transactional memory.
 * 
 * Each region centrally manages per-TX rollback.
 *     TX | Trace
 *      I | W W … R W
 *     II |           R W
 * 1. One cannot defer writes to epoch end because
 *    1.1 Writes are from temporary buffers in user memory. Contents of the
 *        buffers may have changed when the R/W TX commits.
 *    1.2 (Point 3 of snapshot isolation) TX I has a read in the end. If the
 *        read targets a previously written word, writes cannot be deferred.
 *        This is because a TX must see its own modifcations.
 * 2. It seems impossible to avoid write rollback. Suppose all ops but the last
 *    write of TX I succeeds. TX I aborts. All previous writes must be rolled
 *    back. Otherwise, atomicity will be violated.
 * 3. When rolling back writes, copy from RO to R/W versions. It is infeasible
 *    to "remember" initial contents of the R/W version alone.
 * 4. Do not write to both copies of memory words. There may be outstanding RO
 *    TXs after the R/W TX.
**/
struct region
{
    struct batcher_t batcher;
    shared_t start;      // Start of (non-free-able) first segment
    atomic_flag insert;  // Guard of segment linked list
    segment_list allocs; // Dynamically allocated shared segments
    size_t size;         // Size of (non-free-able) first segment
    size_t align;        // Global alignment, i.e., size of a word
    // Log for write rollback
    bool write[MAX_RW_TX]; // No. of R/W TX capped at `MAX_RW_TX`
    wrecord* whistory[MAX_RW_TX];
};

/** Initialize the thread batcher; called by `tm_create`.
 * @param batcher Thread batcher to initialize
 * @return Whether the operation is a success
**/
bool batcher_init(struct batcher_t* batcher);

/** Clean the thread batcher up; called by `tm_destroy`.
 * @param batcher Thread batcher to clean up
**/
void batcher_cleanup(struct batcher_t* batcher);

/** Get epoch ID.
 * @param batcher Thread batcher to get epoch ID from
 * @return Epoch ID
**/
int batcher_get_epoch(struct batcher_t* batcher);

/** Wait and enter a batch.
 * @param batcher Thread batch to enter
 * @param is_ro   Whether the TX is read-only
 * @return TX ID; -1 if R/W TX no. exceeds `MAX_RW_TX`
**/
int batcher_enter(struct batcher_t* batcher, bool is_ro);

/**
 * @brief Leave the current batch.
 * 
 * The interface is inconsistent with the reference implementation. A TM handle
 * is added. This is because it is the batcher's responsiblity to free marked
 * segments after the last TX exits the current epoch.
 * 
 * `tm_end` cannot assume such responsibility. Should `tm_end` were to free the
 * marked segments, no segments would be freed in the case where the last TX in
 * the current batch aborts. Aborted transactions will not call `tm_end`.
 * 
 * @param shared Shared memory region to leave
 * @param batcher Thread batch to leave
**/
void batcher_leave(shared_t shared, struct batcher_t* batcher);
