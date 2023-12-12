#include <stdatomic.h>

// Atomic variable as spinlock

/** Acquire spinlock.
 * @param lock Pointer to atomic variable
**/
static inline void acquire(atomic_flag *lock) {
    while (atomic_flag_test_and_set(lock)); // Spin
}

/** Release spinlock.
 * @param lock Pointer to atomic variable
**/
static inline void release(atomic_flag *lock) {
    atomic_flag_clear(lock);
}
