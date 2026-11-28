#!/usr/bin/env bash
set -euo pipefail

SRC="problem1.c"
BIN="./problem1_test"
CC_FLAGS="-std=gnu11 -Wall -Wextra -pthread"

if [ ! -f "$SRC" ]; then
    echo "Error: $SRC not found. Run this script from the directory containing problem1.c."
    exit 1
fi

trap 'rm -f "$BIN"' EXIT

run_case() {
    local name="$1"
    local runs="$2"
    shift 2

    echo "=== $name ($runs runs) ==="
    gcc $CC_FLAGS "$@" "$SRC" -o "$BIN"

    for ((i = 1; i <= runs; i++)); do
        timeout 5 "$BIN" > /dev/null
    done

    echo "PASS: $name ($runs/$runs)"
}

# Sample baseline (N=1, M=1, P=1, W=1, C=1, K=2 -> 2 items)
run_case "Sample baseline (1,1,1,1,1,2)" 10 \
    -DBUFFER_A_SIZE=1 -DBUFFER_B_SIZE=1 \
    -DNUM_PRODUCERS=1 -DNUM_WORKERS=1 -DNUM_CONSUMERS=1 \
    -DITEMS_PER_PRODUCER=2

# N=1, M=1, P=3, W=3, C=3, K=30 -> 90 items
run_case "Row 1: Small buffers stress (1,1,3,3,3,30)" 50 \
    -DBUFFER_A_SIZE=1 -DBUFFER_B_SIZE=1 \
    -DNUM_PRODUCERS=3 -DNUM_WORKERS=3 -DNUM_CONSUMERS=3 \
    -DITEMS_PER_PRODUCER=30

# N=2, M=2, P=4, W=2, C=4, K=15 -> 60 items, Worker 2ms
run_case "Row 2: Slow workers (2,2,4,2,4,15)" 10 \
    -DBUFFER_A_SIZE=2 -DBUFFER_B_SIZE=2 \
    -DNUM_PRODUCERS=4 -DNUM_WORKERS=2 -DNUM_CONSUMERS=4 \
    -DITEMS_PER_PRODUCER=15 -DWORK_DELAY_US=2000

#N=2, M=2, P=2, W=4, C=2, K=15 -> 30 items, Producer 2ms
run_case "Row 3: Slow producers (2,2,2,4,2,15)" 10 \
    -DBUFFER_A_SIZE=2 -DBUFFER_B_SIZE=2 \
    -DNUM_PRODUCERS=2 -DNUM_WORKERS=4 -DNUM_CONSUMERS=2 \
    -DITEMS_PER_PRODUCER=15 -DPROD_DELAY_US=2000

#N=2, M=2, P=2, W=4, C=2, K=15 -> 30 items, Consumer 2ms
run_case "Row 4: Slow consumers (2,2,2,4,2,15)" 10 \
    -DBUFFER_A_SIZE=2 -DBUFFER_B_SIZE=2 \
    -DNUM_PRODUCERS=2 -DNUM_WORKERS=4 -DNUM_CONSUMERS=2 \
    -DITEMS_PER_PRODUCER=15 -DCONS_DELAY_US=2000

# N=1, M=2, P=5, W=3, C=2, K=50 -> 250 items
run_case "Row 5: Heavy wraparound (1,2,5,3,2,50)" 20 \
    -DBUFFER_A_SIZE=1 -DBUFFER_B_SIZE=2 \
    -DNUM_PRODUCERS=5 -DNUM_WORKERS=3 -DNUM_CONSUMERS=2 \
    -DITEMS_PER_PRODUCER=50

echo "All 110 Problem 1 test runs PASSED."
