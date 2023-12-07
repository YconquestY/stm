#pragma once

#include <pthread.h>
#include <stdbool.h>

/**
 * @brief 
 */
struct batcher_t {
    int counter;   // Current epoch ID
    int remaining; // No. of unfinished threads in current epoch
    int blocked;   // No. of waiting threads, to be executed in next epoch
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/** Initialize the thread batcher.
 * @param batcher Thread batcher to initialize
 * @return Whether the operation is a success
**/
bool batcher_init(struct batcher_t* batcher);

/** Clean the thread batcher up.
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
 * @return Whether the operation is a success
**/
//bool batcher_enter(struct batcher_t* batcher);
void batcher_enter(struct batcher_t* batcher);

/** Leave the current batch.
 * @param batcher Thread batch to leave
**/
void batcher_leave(struct batcher_t* batcher);
