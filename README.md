# linux2026-stdc

Homework 2 for [Linux Kernel Design (Spring 2026)](https://wiki.csie.ncku.edu.tw/linux/schedule).

Assignment: [linux2026-stdc](https://hackmd.io/@sysprog/linux2026-stdc)

## Structure

```
├── hackmd.md          # HackMD report (synced via GitHub Sync)
├── cache_exp.c        # Cache behavior experiment: fast-slow vs single pointer
├── test_cache_exp.c   # Unity tests for cache_exp functions
├── unity.{c,h}        # Unity test framework (ThrowTheSwitch v2.6.2)
├── run_exp.sh         # Automated benchmark runner (auto-detects perf)
├── plot_exp.py        # Generate SVG charts from benchmark results
├── Makefile           # Build, test, benchmark, and plot
└── README.md
```

## Quick Start

```bash
# Build and run tests
make test

# Run benchmarks (auto-detects perf stat if available)
make bench

# Generate charts from benchmark results
make plot
```

## Experiment: Cache Behavior of Linked List Traversal

Compares two algorithms for finding the middle node of a singly linked list:

1. **Fast-slow pointer** — two pointers advance in the same loop; the slow
   pointer visits nodes recently accessed by the fast pointer (good temporal
   locality).
2. **Single pointer (two-pass)** — first pass counts all nodes, second pass
   walks to the middle; cache lines from the first half may be evicted during
   the first pass through the second half.

Reference: [Analysis of Fast-Slow Pointers](https://hackmd.io/@sysprog/ry8NwAMvT)

### What it measures

- Wall-clock timing comparison across list sizes (1K to 10M nodes)
- Hardware cache miss rates via `perf stat` (when available)
- Two memory layouts: sequential (good spatial locality) and shuffled (poor
  spatial locality, simulates real-world heap allocation)

### Requirements

- GCC
- `perf stat` with hardware counter access (optional; falls back to timing)
- Python 3.12+ with `uv` or `pip` (for plotting; uses matplotlib and pandas)

## License

MIT (Unity test framework)
