# CS-453 Project

what is the use of `tx_t`?

## Notes

1. [`macros.h`](./template/macros.h) includes macros for compiler optimization towards branch prediction.
   - `likely(…)` specifies a condition more likely to happen;
   - `unlikely(…)`specifies a condition **less** likely to happen;
   - `unused(…)` tells the compiler that a variable may not be called (although declared) to avoid warnings.

why enclosing `pthread_cond_wait` with a `while` loop?

why `…_t` when customizing a type?

is it necessary to lock `batcher.epoch`?

1st epoch of a shared mem region: onlt $1$ thread in batch?

is it possible for `tm_begin(…)` to return `invalid_tx`?

the library does not scale.

per-word *access set*:
1. bitmap represented by `uint64_t`: $\le 64$ tx per epoch?
2. hash set

read and write size <font color="red">cannot</font> exceed that of the target segment

abort: `tm_end(…)` will not be called $\implies$ garbage collection

`tm_alloc`: when to return `abort_alloc`?

summary of data race
1. multiple segments inserts to head of region

How to notify region of segment deregistration (to avoid linked list walk)?
optimization: precise segment deallocation instead of segment list walk?
optimization: `batcher.dereg` need not be atomic?

Same segment: concurrent free and read/write: undefined behavior?

r/w tx total order: concurrent r/w $\xrightarrow[]{\verb|batcher.remaining == 0|}$ free segment $\rightarrow$ swap

`tm_free(…)` never returns `false` because segment deregistration is deferred until epoch end.
