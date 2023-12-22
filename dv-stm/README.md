# CS-453 Project

This file records the design philosophy of DV-STM.

## Overview

### API

The APIs of DV-STM are defined in [`tm.h`](https://github.com/YconquestY/stm/blob/main/include/tm.h). They are

| API | Description |
| --- | ---         |
| `shared_t tm_create(size_t, size_t);` | Create a (shared) memory *region* |
| `void tm_destroy(shared_t);` | Destroy the memory *region* |
| `void* tm_start(shared_t);` | Get the address of the first word of the first *segment* of the memory *region* |
| `size_t tm_size(shared_t);` | Get the size (in bytes) of the first *segment* of the memory *region* |
| `size_t tm_align(shared_t);` | Get the alignment requirement of the memory *region* |
| `tx_t tm_begin(shared_t, bool);` | Begin a transaction |
| `bool tm_end(shared_t, tx_t);` | End the transaction |
| `bool tm_read(shared_t, tx_t, void const*, size_t, void*);` | Read word(s) |
| `bool tm_write(shared_t, tx_t, void const*, size_t, void*);` | Write word(s) |
| `alloc_t tm_alloc(shared_t, tx_t, size_t, void**);` | Allocate a new memory *segment* |
| `bool tm_free(shared_t, tx_t, void*);` | Destroy the memory *segment* |

Note that reads and writes are always through a (temporary) buffer. Bytes are directly copied. It is impossible to simply write values to DV-STM.

### Layout

DV-STM is already specified [here](https://dcl.epfl.ch/site/_media/education/ca-project.pdf). It is logically laid out as below.

![Logical layout](https://github.com/YconquestY/stm/blob/main/dv-stm/assets/layout.png)

A shared memory *region* contains several variable-size *segments*, as well as the corresponding control structures. Each segment holds variable-number *words*. A memory word holds `align` bytes.

The control structures include a thread batcher, a stack to assign IDs to allocated segments, operating history of read/write transactions, and miscellaneous flags.

## Design

### Shared memory region

The illustration below details the layout of a memory region.

![Region implementation](https://github.com/YconquestY/stm/blob/main/dv-stm/assets/region.png)

The DV-STM library itself does **not** spawn threads. Instead, the library users may create multiple threads accessing the shared region. Each user thread may contain $1$ or more transaction. Transactions are categorized as read-only ones (RO TXs) and read-write ones (R/W TXs). RO TXs access the read-only version of a segment, whereas R/W TXs access the read-write version of a segment.

TXs execute in *epochs*. In every epoch, a *batch* of TX (blocked in the previous epoch) proceed. The thread batcher assigns IDs to TXs in a batch. As explained [below](#segment-implementation), there are at most $63$ R/W TXs in a batch. Extra R/W TXs will be rejected, and it is the **user**'s responsible to retry such a TX. There is no number limit on RO TXs. R/W TX IDs range from $0$ to $62$, and RO TX IDs always start from $63$.

With a word address, multiple TXs may access a segment concurrently. I implemented another abstraction of *opaque address* over the (virtual) process address space, which is illustrated below.

![Opaque address](https://github.com/YconquestY/stm/blob/main/dv-stm/assets/void_.png)

By [spec](https://dcl.epfl.ch/site/_media/education/ca-project.pdf), a segment holds $\le 2^{48}$ bytes, addressing which will require $48$ bits. In contrast, a `void*` is $64$ bits, which means there are $2$ bytes unused! I let $6$ bits (bits $48 \rightarrow 53$) to represent the segment ID. In this way, given an arbitrary opaque address, I may know 1) which segment it refers to, as well as 2) the offset from the first word in the segment. Bits $54 \rightarrow 63$ are still "wasted". As an optimization, they may be flags for invalidities.

When allocating a new segment, the library consults the segment ID stack top to assign an unused ID. The ID functions as an index to the `allocs` segment table, which holds pointers to segments. The opaque address gives $6$ bits as segment ID, so `allocs` has $2^6 = 64$ (actually $63$) entries. A TX that `tm_alloc(…)` extra segments must abort, and it is the library **user**'s responsiblity to retry it. A maximum of $63$ segments is enough for grading purpose. To support more segments per region, allocate more bits in the `void*` opaque address as segment ID, and enlarge the `allocs` array.

All operations in a R/W TX prior to the abort point must be rolled back. Otherwise, atomicity would be violated. `history` records the behavior of every R/W TX. If the TX commits, its history is cleared. In contrast, an aborted TX must restore the segment according to its own history.

### Segment implementation

The illustration below details the layout of a segment.

![Segment implementation](https://github.com/YconquestY/stm/blob/main/dv-stm/assets/segment.png)

A segment comprises a read-only version of words, a read-write version, and per-word control structures.

![Per-word control](https://github.com/YconquestY/stm/blob/main/dv-stm/assets/aset.png)

A memory word is primarily controlled by an *access set* (illustrated above). An access set is an `uint64_t` flag. As [aforementioned](#shared-memory-region), each batch supports up to $63$ R/W TX, each cooresponding to a bit in an `uint64_t`. In bits $0 \rightarrow 62$, a bit is set to $\verb|1|$ whenever a R/W TX accesses the word. Bit $63$ is reserved to imply whether a word has been written.

## Problems encountered in the project

- Exception: `Transactional library takes too long to process the transactions`<br>
  Always use the address (`struct batcher_t*`) of the thread batcher. Do not declare another instance (`struct batcher_t`).<br>
  Use epoch count rather than the number of outstanding TXs as `pthread_cond_broadcast` criterion.
  - Problem still sporadically triggered for **unknown** reason
- Segmentation fault
  - Forgot to exclude RO TX when accessing op history
  - The first segment is accidentally freed.<br>
    Use `atomic_bool` rather than `atomic_flag`.
  - An empty history has non-`NULL` `history[ID]`
- `Violated isolation or atomicity`<br>
  Follow exactly what the algorithm does. Do not make unproved equivalents of access sets

## What I've learned from the project

- A taste of C atomics
  - How to use an `atomic_flag` as a lock
- Compiler hints (see [`macros.h`](https://github.com/YconquestY/stm/blob/main/dv-stm/macros.h))
- Spotting segmentation faults with gdb
- Detecting memory leak with Valgrind
