CC = gcc
CFLAGS = -Wall -Wextra -O2

# Find uv or python3 for plotting
PYTHON := $(shell command -v uv >/dev/null 2>&1 && echo "uv run" || echo "python3")

.PHONY: all test bench plot plot-combined clean

all: cache_exp

# --- Build targets ---
cache_exp: cache_exp.c
	$(CC) $(CFLAGS) -o $@ $<

test_cache_exp: test_cache_exp.c cache_exp.c unity.c
	$(CC) $(CFLAGS) -DTEST_BUILD -o $@ $^

# --- Workflows ---

# Run unit tests
test: test_cache_exp
	./test_cache_exp

# Run benchmarks (auto-detects perf stat)
bench: cache_exp
	./run_exp.sh

# Generate chart from local results
plot: results.csv
	$(PYTHON) plot_exp.py

# Generate per-machine + combined comparison charts
# Place each machine's results in results_<name>/ first
plot-combined:
	$(PYTHON) plot_combined.py

results.csv: cache_exp run_exp.sh
	./run_exp.sh

clean:
	rm -f cache_exp test_cache_exp results.csv sysinfo.txt
	rm -f cachegrind.out.*
