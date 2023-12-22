# DV-STM

A dual-versioned **software transactional memory** library with over $3 \times$ speedup against the reference implementation using a coarse-grained lock.

## TODO

- [ ] To understand the difference between `inline …` and `static inline …`<br>
  and why
  ```
  cc -Wall -Wextra -Wfatal-errors -O2 -std=c11 -fPIC -I../include -c -o batcher.c.o batcher.c
  batcher.c:268:20: error: static declaration of ‘acquire’ follows non-static declaration
    268 | static inline void acquire(atomic_flag* lock) {
        |                    ^~~~~~~
  compilation terminated due to -Wfatal-errors.
  ```
- [ ] To understand the behavior of [`posix_memalign(…)`](https://man7.org/linux/man-pages/man3/posix_memalign.3.html) regarding virtual address and alignment
- [ ] To understand dynamic allocation of `struct` with `union`
- [ ] To optimize "access sets" update<br>
  Currently, per-word "access sets" are always updated during each read/write operation even though it is **not** necessary to do so.
- [ ] To understand synchronization examples in [`sync-examples/`](https://github.com/YconquestY/stm/tree/main/sync-examples)
- [ ] To understand examples in [`playground/`](https://github.com/YconquestY/stm/tree/main/playground)
- [ ] To understand the workload and how an implementation is graded in [`grading/`](https://github.com/YconquestY/stm/tree/main/grading)

## Introduction

The [project description](https://dcl.epfl.ch/site/_media/education/ca-project.pdf) includes

- An introduction to STM;
- The specifications of the STM implemented;
  - Sufficient properties for an STM to be deemed correct
  - The DV-STM algorithm
  - A thorough description of the STM interface
- Practical information.
  - How to test an implementation locally
  - The performance metric

## Directory organization

| Directory | Description |
| ---       | ---         |
| [`dv-stm/`](https://github.com/YconquestY/stm/tree/main/dv-stm) | DV-STM implementation |
| [`grading/`](https://github.com/YconquestY/stm/tree/main/grading) | Workload and grader |
| [`include/`](https://github.com/YconquestY/stm/tree/main/include) | STM API |
| [`playground/`](https://github.com/YconquestY/stm/tree/main/playground) | Unknown |
| [`reference/`](https://github.com/YconquestY/stm/tree/main/reference) | A reference implementation using a coarse-grained lock |
| [`sync-examples/`](https://github.com/YconquestY/stm/tree/main/sync-examples) | Examples on synchronization primitives |
| [`submit.py`](https://github.com/YconquestY/stm/blob/main/submit.py) | Autograding submission script |

## Acknowledgement

This project is the coursework of [CS-453](https://dcl.epfl.ch/site/education/ca_2023) by [École Polytechnique Fédérale de Lausanne](https://www.epfl.ch) (EPFL).
