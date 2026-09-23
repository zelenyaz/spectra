# mbench

A multi-threaded memory-performance micro-benchmark using **scalar 64-bit load/store instructions**. It runs several independent benches concurrently, each with its own buffer, access pattern, operation mix, thread count, and optional NUMA placement / CPU pinning, and reports throughput every second.

## Requirements

- x86-64 Linux (the load/store path uses `movq` inline assembly)
- `g++` with C++17
- `libnuma` (`libnuma-dev` / `numactl-devel`)

## Build

```sh
make            # -> ./mbench   (g++ -O3 -march=native -std=c++17 -pthread … -lnuma)
make clean
```

## Usage

```
./mbench --benches "<spec>[,<spec>...]" [--time SECONDS] [--numa-csv=FILE[:S]]
```

Each `<spec>` is `name:threads[:key=value ...]`:

- `name = <order>_<op>`
- `threads` — number of threads for this bench
- zero or more `key=value` options

Benches are separated by commas; fields within a bench by colons.

### Access orders

| order        | parameters     | pattern |
|--------------|----------------|---------|
| `stride`     | `ssz`          | strided walk of each thread's buffer with stride `ssz` (uint64_t); covers every element via the off-loop `for off in 0..ssz-1: for e=off; e<n; e+=ssz` |
| `stride_pf`  | `ssz`,`pd`     | `stride` variant that issues a software prefetch `pd` future strided accesses ahead; the bounded prefix never prefetches outside the thread's buffer |
| `stridep`    | `np`           | same kernel, but the stride is derived from a partition count: `stride = ceil(elems_per_thread / np)` |
| `chk_stride` | `ssz`,`x`,`y`  | working-set-reuse variant: split the buffer into chunks of `x` bytes; do `y` full strided (`ssz`) sweeps of each chunk before advancing |
| `rand`       | —              | uniform random index per access via `xorshift64` + Lemire multiply-shift (no modulo) |

### Operations

| op      | meaning |
|---------|---------|
| `read`  | load a uint64_t |
| `write` | store a uint64_t |
| `rw`    | read a uint64_t, then write it (back) |
| `wr`    | write a uint64_t, then read it (same address) |

Any order × op combination is valid, e.g. `stride_read`, `stride_pf_read`, `stridep_rw`, `chk_stride_write`, `rand_wr`. (Names are split on the **last** `_`, since orders such as `stride_pf` and `chk_stride` contain one.)

### Keys

| key       | applies to        | default | meaning |
|-----------|-------------------|---------|---------|
| `ssz=N`   | stride/stride_pf/chk_stride | `8` | stride in uint64_t elements |
| `pd=N`    | stride_pf         | `16`    | prefetch distance in future strided accesses |
| `np=N`    | stridep           | `16`    | partition count → stride |
| `x=N`     | chk_stride        | `1M`    | chunk size in bytes (`K`/`M`/`G` suffix) |
| `y=N`     | chk_stride        | `20`    | times each chunk is reused before advancing |
| `size=N`  | all               | `256M`  | total buffer bytes (`K`/`M`/`G`); rounded down to a multiple of `threads*64` |
| `bufnode=N` | all             | none    | bind the buffer to NUMA node `N` (via `mbind`) |
| `cpunode=N` | all             | none    | pin the bench's threads to the cores of NUMA node `N` |
| `cpus=LIST` | all             | none    | pin the bench's threads to an explicit core list (overrides `cpunode`); `LIST` is `+`-separated cores/ranges, e.g. `cpus=0-3` or `cpus=0+2+4`. Round-robin if `threads` > cores. `+` (not `,`) separates cores since `,` separates benches and `:` separates fields |

`--time SECONDS` sets the total run time (default `10`).

`--numa-csv=FILE[:S]` enables the NUMA placement monitor — see below.

## NUMA placement monitor

`--numa-csv=FILE[:S]` (also `--numa-csv FILE[:S]`) spawns a helper thread that, every `S` seconds (integer, default `3`), queries where each bench buffer's pages physically reside — via `move_pages()` with `nodes=NULL`, which only reads placement and never migrates anything — and appends the per-node page tally to `FILE` as CSV. This makes page migration over the run (e.g. AutoNUMA) visible.

The token is split on its **last** `:`; the suffix is taken as the interval only if it is all digits, so paths that contain a `:` still parse as a path.

The CSV has one header row and one row per bench per sample (wide layout):

```
time_s,bench,total_pages,total_MiB,node0,node1,...,nodeK,unknown
3.0,stride_read,131072,512.0,0,131072,0
3.0,rand_write,65536,256.0,65536,0,0
```

- `time_s` — seconds since the timed run started (matches the stdout reporter).
- `total_pages` / `total_MiB` — buffer size in system pages (`getconf PAGE_SIZE`, typically 4 KiB) and MiB.
- `node0 … nodeK` — resident page count per NUMA node (`K = numa_max_node()`).
- `unknown` — pages whose status is not a valid node (not-yet-present / error).

The page-table walk runs in a separate thread every `S` seconds and perturbs the measurement only momentarily. Transparent huge pages are reported per base page, so a 2 MiB page contributes 512 counts on its node — tallies stay correct.

## Buffers, NUMA, and pinning

- Each bench gets its **own** buffer, allocated with `mmap` (page-aligned, so ≥ 64-byte aligned) and split **equally** among its threads; threads never touch outside their own sub-range.
- With `bufnode=N`, the buffer is `mbind`-bound to node `N`, faulted so the physical pages land there, then the range's policy is restored to the system default. Without it, pages are first-touched by the owning (pinned) thread.
- With `cpunode=N`, thread *t* is pinned to `cores_of_node_N[t % count]` (round-robin). With `cpus=LIST` (which overrides `cpunode`), thread *t* is pinned to `LIST[t % len(LIST)]`, where `LIST` is a `+`-separated set of cores and `A-B` ranges (e.g. `cpus=0-3`). The chosen cores are printed per bench. Use `cpus=` to give concurrent benches **disjoint** core sets (e.g. `…:cpus=0-3,…:cpus=4-7`); `cpunode` cannot, since every bench round-robins from the node's first core and so overlaps.

## Output

Per bench, at startup: buffer start address + size, per-thread chunk, the effective stride/chunk parameters, the bound NUMA node, and the pinned cores.

Then every second, and as a final total, per bench:

```
[  3.0s] stride_read          826.16 Mops/s      6.61 GB/s
```

One **op = one 64-bit (8-byte) memory access**: `read`/`write` count 1 op per element, `rw`/`wr` count 2. `GB/s = ops × 8 / seconds` (decimal GB).

## Examples

```sh
# Sequential-ish read vs random write, separate buffers, 4 and 6 threads:
./mbench --benches "stride_read:4:ssz=8:size=512M,rand_write:6:size=1G" --time 20

# NUMA local vs remote read (buffer on node 0, threads on node 0 then node 1):
./mbench --benches "stride_read:4:size=512M:bufnode=0:cpunode=0" --time 5
./mbench --benches "stride_read:4:size=512M:bufnode=0:cpunode=1" --time 5

# Cache-reuse pattern: 256 KiB chunk reused 50× vs cache-missing baseline:
./mbench --benches "chk_stride_read:4:x=256K:y=50:size=1G,stride_read:4:size=1G" --time 10

# Compare demand-only stride with software prefetching 16 accesses ahead:
./mbench --benches "stride_read:4:ssz=8:size=1G,stride_pf_read:4:ssz=8:pd=16:size=1G" --time 10
```

## Scalar 64-bit guarantee

Demand loads/stores go through `movq` inline assembly (`ld64`/`st64`), which the optimizer cannot vectorize or elide. `stride_pf` additionally emits scalar software-prefetch instructions. To confirm the kernels use no vector registers and the `stride_pf` wrappers contain prefetch instructions:

```sh
objdump -d mbench | awk '/^[0-9a-f]+ <_Z(11run_strided|14run_strided_pf|14run_chk_stride|10run_random)I/{f=1} /^$/{f=0} f' | grep -Ei '%xmm|%ymm|%zmm'
# vector-register check above should print nothing
objdump -d mbench | awk '/^[0-9a-f]+ <_Z14run_strided_pfI/{f=1} /^$/{f=0} f' | grep prefetch
# prefetch check above should print instructions
```

## Notes

- Pinning two benches to overlapping cores oversubscribes them; give each bench distinct cores, or keep its thread count ≤ the node's core count, for clean numbers.
