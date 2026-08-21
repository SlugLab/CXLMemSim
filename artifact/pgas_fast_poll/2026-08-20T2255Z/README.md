# PGAS fast-poll live evidence

## Configuration

The server ran the synchronous PGAS shared-memory protocol with one polling
worker and transport-only accounting:

```text
./build/cxlmemsim_server --comm-mode=pgas-shm \
  --pgas-shm-name=/cxlmemsim_pgas --capacity=256 \
  --pgas-workers=1 --pgas-spin-us=1000 --pgas-yield-count=10 \
  --pgas-idle-sleep-us=100 --pgas-record-accesses=false
```

`record-accesses=false` skips the controller's linear per-address occupation
history. It does not skip address validation, data reads/writes, synchronous
responses, or protocol counters.

The guest allocator mapped the working buffer `rw-s` from `/dev/dax0.0`, and
the device remained bound to `/sys/bus/dax/drivers/device_dax`.

## Adaptive-client result

The exact guest command was:

```text
LD_PRELOAD=target/debug/libcxlalloc_preload.so \
  /root/lmbench/bin/x86_64-linux-gnu/lat_mem_rd -t -N 4 128 64
```

The adaptive QEMU client completed all 115 numeric rows with no zero values and
exit status 0. Its final row was:

```text
128.00000 5009.636
```

QEMU reported a 497.7 ns average successful server-response wait after
771,000,000 operations. It also reported one isolated 100 millisecond response
timeout; this is retained as a limitation even though the benchmark completed.

## Original-wait mediated control

The same QEMU integration checkout was rebuilt with the adaptive wait reverted.
To avoid repeating the full logarithmic sweep, only the 128 MiB endpoint was
selected while retaining `-N 4`:

```text
LD_PRELOAD=target/debug/libcxlalloc_preload.so \
  /root/lmbench/bin/x86_64-linux-gnu/lat_mem_rd -t -N 4 \
  -s 134217728 -e 134217728 128 64
```

That control exited 0 at:

```text
128.00000 31963.106
```

Server reads advanced from 404,900,000 to 430,000,000 and writes from
370,898,371 to 400,260,303 during this endpoint, proving mediation. Relative
to this control, adaptive polling reduced endpoint latency by 84.3%.

## Rejected direct control

An installed-QEMU run completed at 4062.530 ns/load, but the server log and
counters did not advance during that run. It is retained as a non-mediated
devdax lower-bound reference and is not used as evidence of server-path
performance.

## Correctness boundary

- Adaptive run: 115 numeric rows, zero zero-valued rows, exit status 0.
- Original-wait endpoint: one numeric row, exit status 0.
- `/dev/dax0.0` remained in `devdax` mode.
- No invalid-opcode trap or `lat_mem_rd` segfault appeared in either accepted
  mediated run.

The remaining approximately 5 microseconds includes KVM MMIO exit, QEMU
dispatch, and a synchronous memory-server round trip. The approximately 4
microsecond non-mediated result is not an achievable server-path target without
changing the per-load mediation requirement.
