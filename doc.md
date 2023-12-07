# CS-453 Project

what is the use of `tx_t`?

## Notes

1. [`macros.h`](./template/macros.h) includes macros for compiler optimization towards branch prediction.
   - `likely(…)` specifies a condition more likely to happen;
   - `unlikely(…)`specifies a condition **less** likely to happen;
   - `unused(…)` tells the compiler that a variable may not be called (although declared) to avoid warnings.

why enclosing `pthread_cond_wait` with a `while` loop?

why `…_t` when customizing a type?
