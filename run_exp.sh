#!/bin/bash
# run_exp.sh - Run cache behavior experiments and collect data
#
# Auto-detects the best available profiling tool:
#   1. perf stat    (hardware counters, most accurate)
#   2. cachegrind   (simulation, works in containers without perf access)
#   3. timing only  (wall-clock time, always works)
#
# Usage:
#   ./run_exp.sh              # auto-detect best tool
#   ./run_exp.sh --no-perf    # skip perf, try cachegrind or timing
#   ./run_exp.sh --timing     # timing only
#
# Output: results.csv + sysinfo.txt

set -e

PROG="./cache_exp"
CSV="results.csv"
SYSINFO="sysinfo.txt"

# --- Detect profiling tool ---
find_perf() {
    if command -v perf &>/dev/null; then
        if perf stat -- true 2>&1 | grep -q 'cycles'; then
            echo "perf"; return
        fi
    fi
    for p in /usr/lib/linux-tools/*/perf; do
        if [ -x "$p" ] && "$p" stat -- true 2>&1 | grep -q 'cycles'; then
            echo "$p"; return
        fi
    done
    echo ""
}

PERF=""
CACHEGRIND=""
MODE="timing"

if [ "$1" = "--timing" ]; then
    : # timing only
elif [ "$1" = "--no-perf" ]; then
    if command -v valgrind &>/dev/null; then
        CACHEGRIND="valgrind"
        MODE="cachegrind"
    fi
else
    PERF=$(find_perf)
    if [ -n "$PERF" ]; then
        MODE="perf"
    elif command -v valgrind &>/dev/null; then
        CACHEGRIND="valgrind"
        MODE="cachegrind"
    fi
fi

# --- Collect system info ---
{
    echo "hostname: $(hostname)"
    echo "kernel: $(uname -r)"
    echo "arch: $(uname -m)"
    echo "cpu: $(lscpu | grep 'Model name' | sed 's/.*:\s*//')"
    echo "cores: $(nproc)"
    lscpu | grep -i 'cache' | sed 's/^\s*//'
    echo "profiler: $MODE"
    echo "date: $(date -Iseconds)"
} > "$SYSINFO"

echo "=== System Info ==="
cat "$SYSINFO"
echo ""

# --- Configuration ---
SIZES=(1000 5000 10000 50000 100000 500000 1000000 5000000 10000000)

get_iters() {
    local size=$1 mode=$2
    if [ "$mode" = "cachegrind" ]; then
        # cachegrind is ~50x slower, use fewer iters
        if   [ "$size" -le 5000 ];    then echo 100
        elif [ "$size" -le 50000 ];   then echo 10
        elif [ "$size" -le 500000 ];  then echo 3
        else echo 1
        fi
    else
        if   [ "$size" -le 5000 ];    then echo 50000
        elif [ "$size" -le 50000 ];   then echo 5000
        elif [ "$size" -le 500000 ];  then echo 500
        elif [ "$size" -le 5000000 ]; then echo 50
        else echo 10
        fi
    fi
}

# --- Run experiments ---
echo "size,mode,algo,time_ns_per_iter,cache_misses,cache_refs,iters" > "$CSV"

for size in "${SIZES[@]}"; do
    iters=$(get_iters "$size" "$MODE")

    for layout in seq shuf; do
        # Check if allocation fits in available memory
        # shuf: individual malloc + spacers (~2KB avg per node)
        # seq: dense calloc (16 bytes per node)
        if [ "$layout" = "shuf" ]; then
            mem_mb=$(( size * 2048 / 1024 / 1024 ))
        else
            mem_mb=$(( size * 16 / 1024 / 1024 ))
        fi
        avail_mb=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo 2>/dev/null || echo 99999)
        if [ "$mem_mb" -gt $(( avail_mb * 3 / 4 )) ]; then
            printf "  %-4s %-6s (n=%-8d) SKIP: need %d MB, avail %d MB\n" \
                   "$layout" "" "$size" "$mem_mb" "$avail_mb"
            continue
        fi

        for algo in fast single; do
            printf "  %-4s %-6s (n=%-8d iters=%-5d) " \
                   "$layout" "$algo" "$size" "$iters"

            if [ "$MODE" = "perf" ]; then
                output=$("$PERF" stat -e cache-references,cache-misses \
                    "$PROG" "$size" "$layout" "$algo" "$iters" 2>&1)

                time_ns=$(echo "$output" | grep -oP '[\d.]+(?= ns/iter)')
                cache_refs=$(echo "$output" | grep 'cache-references' | \
                    awk '{gsub(/,/,"",$1); print $1}')
                cache_misses=$(echo "$output" | grep 'cache-misses' | \
                    awk '{gsub(/,/,"",$1); print $1}')

                echo "$size,$layout,$algo,$time_ns,$cache_misses,$cache_refs,$iters" >> "$CSV"
                printf "%10s ns/iter  miss=%s/%s\n" \
                       "$time_ns" "$cache_misses" "$cache_refs"

            elif [ "$MODE" = "cachegrind" ]; then
                # Run with cachegrind to get simulated D1 cache stats
                cg_out=$($CACHEGRIND --tool=cachegrind --cache-sim=yes \
                    "$PROG" "$size" "$layout" "$algo" "$iters" 2>&1)

                time_ns=$(echo "$cg_out" | grep -oP '[\d.]+(?= ns/iter)')
                # D refs = total data references (reads + writes)
                # Lines are prefixed with ==PID==, e.g.:
                #   ==12345== D refs:        399,343  (229,354 rd ...
                #   ==12345== D1  misses:     39,033  ( 34,322 rd ...
                d_refs=$(echo "$cg_out" | grep 'D refs:' | \
                    awk '{for(i=1;i<=NF;i++) if($i=="refs:") {gsub(/,/,"",$(i+1)); print $(i+1); break}}')
                # D1 misses = L1 data cache misses
                d1_misses=$(echo "$cg_out" | grep 'D1  misses:' | \
                    awk '{for(i=1;i<=NF;i++) if($i=="misses:") {gsub(/,/,"",$(i+1)); print $(i+1); break}}')

                echo "$size,$layout,$algo,$time_ns,$d1_misses,$d_refs,$iters" >> "$CSV"
                if [ -n "$d_refs" ] && [ "$d_refs" -gt 0 ] 2>/dev/null; then
                    miss_pct=$(awk "BEGIN {printf \"%.1f\", $d1_misses * 100.0 / $d_refs}")
                    printf "%10s ns/iter  D1 miss=%s/%s (%s%%)\n" \
                           "${time_ns:-n/a}" "$d1_misses" "$d_refs" "$miss_pct"
                else
                    printf "%10s ns/iter  D1 miss=%s/%s\n" \
                           "${time_ns:-n/a}" "$d1_misses" "$d_refs"
                fi

            else
                time_ns=$("$PROG" "$size" "$layout" "$algo" "$iters" 2>/dev/null | \
                    grep -oP '[\d.]+(?= ns/iter)')

                echo "$size,$layout,$algo,$time_ns,,,$iters" >> "$CSV"
                printf "%10s ns/iter\n" "$time_ns"
            fi
        done
    done
done

echo ""
echo "Profiler: $MODE"
echo "Results saved to $CSV"
echo "System info saved to $SYSINFO"
