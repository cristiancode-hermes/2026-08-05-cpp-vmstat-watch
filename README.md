# vmstat-watch — Linux Virtual Memory Statistics Monitor

A C++23 CLI tool that reads `/proc/vmstat` and displays categorized Linux kernel virtual memory statistics.

## Features

- **7 Categories**: Page Faults, Swap, NUMA, Workingset, Slab, Compaction, THP
- **50+ Counters**: pgfault, pgmajfault, pswpin, pswpout, numa_hit, numa_miss, workingset_refault, compact_migrate_scanned, thp_fault_alloc and more
- **Human Formatting**: Large values in B/M/K (14.3B, 763.6M, 34.1K)
- **Watch Mode**: `--watch N` with periodic refresh, deltas and per-second rates
- **JSON Output**: `--json` for scripting with all 178+ counters sorted alphabetically
- **Self-Test**: `--selftest` with 15 validation cases
- **Zero Dependencies**: Standard library only

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o vmstat-watch
```

## Usage

```bash
# Snapshot (default)
./vmstat-watch

# Watch mode with 3-second refresh
./vmstat-watch --watch 3

# JSON output
./vmstat-watch --json

# Run self-tests
./vmstat-watch --selftest

# Plain text (no ANSI colors)
./vmstat-watch --no-color
```

## Output Categories

| Category | Key Counters |
|----------|-------------|
| Page Faults | pgfault, pgmajfault, pgpgin, pgpgout |
| Swap | pswpin, pswpout |
| NUMA | numa_hit, numa_miss, numa_local, numa_other |
| Workingset | workingset_refault_file, workingset_activate_file |
| Slab | slab_alloc, slab_free, slab_nr_pages |
| Compaction | compact_migrate_scanned, compact_free_scanned, compact_isolated |
| THP | thp_fault_alloc, thp_collapse_alloc, thp_split |
