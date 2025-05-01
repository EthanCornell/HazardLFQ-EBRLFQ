# HazardLFQ

HazardLFQ is an **industrial-strength implementation of the Michael & Scott
lock-free queue** written in modern C++ 20.  
It comes with a **minimal hazard-pointer library**, a **stress-test
suite**, and build recipes that pass both **ThreadSanitizer** and
**AddressSanitizer** on Linux/GCC.

> **Why another LF queue?**  
>  – Because most “textbook” examples gloss over safe memory
>  reclamation. HazardLFQ shows a complete, production-ready solution
>  in ≤ 300 lines of code.

---

## Features

| ✔︎ | Description |
|---|-------------|
| **Header-only** | just add `#include "lockfree_queue.hpp"` |
| **Lock-free (non-blocking)** | progress for at least one thread under contention |
| **Hazard-pointer reclamation** | no ABA, no use-after-free, no epoch library dependency |
| **C++20 & std::atomic** | no compiler intrinsics, portable across GCC / Clang / MSVC |
| **Instrumentation flag** | `-DLFQ_INSTRUMENT` counts live nodes for leak detection |
| **Sanitizer-clean** | passes TSan + ASan with the provided tests |
| **Self-contained tests** | `lockfree_queue_tests.cpp` – run with one command |

---

## Directory layout

```
.
├── lockfree_queue.hpp       # the queue + hazard-pointer header
├── lockfree_queue_tests.cpp # stress & regression tests
└── README.md
```

---

## Quick start

### 1.  Clone & build

```bash
git clone https://github.com/your-org/hazardlfq.git
cd hazardlfq

# ThreadSanitizer build
g++ -std=c++20 -O1 -g -fsanitize=thread \
    -pthread lockfree_queue_tests.cpp -o lfq_tsan

# AddressSanitizer build
g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
    -pthread lockfree_queue_tests.cpp -o lfq_asan
```

### 2.  Run the test-suite

```bash
./lfq_tsan   # or ./lfq_asan
# → All tests PASSED 🎉
```

The suite exercises:

* 32-thread atomic-correctness burst  
* ABA / UAF stress with mixed producers & consumers  
* Full memory-reclamation check (no leaks)  
* Destructor safety race test  
* Live-lock regression watchdog  

### 3.  Use it in your code

```cpp
#include "lockfree_queue.hpp"

LockFreeQueue<int> q;
q.enqueue(42);

int value;
if (q.dequeue(value))
    std::cout << value << '\n';
```

> **Tip:** add `-DLFQ_INSTRUMENT` when you link *your* application if you
> want the `LockFreeQueue<>::live_nodes()` counter for debug builds.

---

## Build matrix

| Compiler | C++ Standard | Sanitizers | Status |
|----------|--------------|------------|--------|
| GCC 13   | C++20        | Thread + Address | ✅ |
| Clang 17 | C++20        | Thread + Address | ✅ |
| MSVC 19  | C++20        | n/a (use `/fsanitize=address` or Dr. Memory) | ✅ |

---

## How it works — in one slide

```
enqueue():                       dequeue():

 tail -----┐                     head ----┐
           ▼                               ▼
 [dummy] -> A -> B -> null        [dummy] -> A -> B -> …
             ↑                           retire(dummy)  (hazard-pointer safe!)
           HP: thread T                 new head
```

* Multiple producers/consumers use `std::atomic<Node*>` with
  `compare_exchange_strong / weak`.
* Every thread exposes at most **two hazard pointers** (`head`, `next`
  **or** `tail`, `next`).
* A node moves through three states: **active → retired → reclaimed**.
* In `hp::scan()` we take a **snapshot of *all* hazard pointers**,
  reclaim everything not in the snapshot.

---

## License

HazardLFQ is released under the **MIT License** – see `LICENSE` for details.

---

## Contributing

Bug reports, portability patches and additional test cases are welcome!
Please open a GitHub issue or PR.
