# CS3013-Group-Project

## Problem 1: Multi-Stage Producer-Consumer System

### Build
From the project root directory, build using `make`:
```bash
make problem1
```

Or compile directly with `gcc`:
```bash
gcc -std=gnu11 -Wall -Wextra -pthread problem1.c -o problem1
```

### Run (Default Configuration)
```bash
./problem1
```

Default macro values in `problem1.c`:
* `BUFFER_A_SIZE=5`, `BUFFER_B_SIZE=5`
* `NUM_PRODUCERS=3`, `NUM_WORKERS=3`, `NUM_CONSUMERS=2`, `ITEMS_PER_PRODUCER=4`
* `PROD_DELAY_US=0`, `WORK_DELAY_US=0`, `CONS_DELAY_US=0`

On success, the parent verifies all child exit statuses and collected results, prints the results table (`id`, `producer_id`, `worker_id`, `result`) in ascending `id` order, and exits with code `0`.

### Customizing Configuration
All configuration macros are wrapped in `#ifndef`, so you can either edit them directly in `problem1.c` and recompile, or override them on the command line with `-D`:
```bash
gcc -std=gnu11 -Wall -Wextra -pthread \
  -DBUFFER_A_SIZE=1 -DBUFFER_B_SIZE=1 \
  -DNUM_PRODUCERS=1 -DNUM_WORKERS=1 -DNUM_CONSUMERS=1 -DITEMS_PER_PRODUCER=2 \
  problem1.c -o problem1
./problem1
```

Optional delay macros (`PROD_DELAY_US`, `WORK_DELAY_US`, `CONS_DELAY_US`, in microseconds) default to `0` and are applied outside critical sections to test different stage speeds.

### Running the Submitted Test Suite
From the project root directory, run:
```bash
chmod +x test_cases/q1/test_problem1.sh
./test_cases/q1/test_problem1.sh
```

This script compiles `problem1.c` into a temporary binary (`./problem1_test`, automatically removed on exit) and executes all 6 report configurations (110 total runs) under `timeout 5`, including minimal `N=1, M=1` buffers, repeated interleavings, artificial stage delays, and heavy circular wraparound.