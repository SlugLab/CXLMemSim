# CXL Type-2 Calibration from C2 Sight (paper.pdf)

Calibrates CXLMemSim's CXL Type-2 device model against the measured SPR + Agilex
testbed characterization in `/root/Splash/paper.pdf`
(*Understanding and Profiling CXL.cache Using C2 Sight*, 25pp).

The paper builds C2 Sight, a what-if differential profiling framework, on a
two-socket Intel Sapphire Rapids server (Xeon Gold 6438Y+, SNC on, cores pinned
4.0 GHz / uncore 2.3 GHz, prefetchers off) with an Agilex 7 R-Tile CXL Type-2
FPGA endpoint (HMC = 256 KiB, FPGA clock 400 MHz). Every calibrated number
below comes from that characterization; each entry carries its paper
provenance (section/figure) and is flagged `reported` (verbatim), `derived`
(sum of reported stage costs, derivation shown), or `cxlmemsim_default`
(paper does not constrain it).

## Files

| File | Purpose |
| --- | --- |
| `paper_measurements.json` | All extracted paper measurements with provenance, incl. the §A.5 CHA address-hash XOR masks |
| `cxlmemsim_calibrated.json` | Calibrated parameter set: legacy CLI + QEMU device props + server flags + coherency stage costs + gaps |
| `run_calibrated_legacy.sh` | Application-level CXLMemSim with the calibrated vectors |
| `launch_qemu_type2_calibrated.sh` | QEMU `cxl-type2` with the calibrated device properties (timing rails on) |
| `qtest_notify_smoke.py` | Timing-model verification: rails + notify command vs expected values (all PASS) |
| `qtest_notify_bench.py` | Notification batch-size microbench (device-model sweep, batch 1→1024) |
| `qtest_sharing_motivation.py` | Motivation experiment: coherent sharing vs explicit publication vs full replication |
| `qtest_scale_bench.py` | Device-count scaling: 1–32 Type-2 endpoints in one QEMU (two pxb buses × 16 root ports) |
| `qtest_dcoh_bench.py` | DCOH allocation backpressure: allocating vs non-allocating working-set streams |
| `run_full_suite.sh` | Full suite runner: functional + smoke + notify + sharing + DCOH + scaling (add `--with-hw` for the real-HW spot check) |
| `notify_bench_results/` | Bench outputs: `notify_bench.json`/`.csv`, `notify_hw.csv`, `sharing_motivation.json`/`.csv`, `scale_bench.csv`, `threads_hw.csv` |
| `make_calibration_graph.py` → `calibration_graph.png`/`.pdf` | The calibration figure |
| `make_motivation_graph.py` → `motivation_graph.png`/`.pdf` | Motivation figure: batching (70×) + sharing vs publication (44×–2775×) |
| `make_scaling_graph.py` → `scaling_graph.png`/`.pdf` | Scaling figure: 1:1 composition to 32 endpoints + physical saturation |

## Device-count and producer scaling (rerun 2026-09-11)

- **Simulated, 32 endpoints** (`qtest_scale_bench.py`): one QEMU hosts
  N ∈ {1,2,4,8,16,32} cxl-type2 endpoints (2 pxb-cxl buses × 16 root ports;
  per-bus PCI addr limit is 18 slots, hence 16 per bus; QEMU requires unique
  `slot=` per cxl-rp and auto-numbers secondary buses in realization order —
  `map_devices` assigns bus 13+j+j//16 and a 64 MB BAR window per device).
  Per-device modeled time is unchanged at every N (16.56 ms notify stream,
  371.4 ns/share-update); aggregate follows the 1:1 line: 8.1 GB/s
  notifications and 86 M shared updates/s at 32 endpoints, 100% efficiency.
- **Physical, producer threads** (`cxl_notify_bench <node> <mode> <count>
  <repeats> <threads> <batch>`): roundtrip aggregate throughput saturates at
  the device — CXL node ~11.5 GB/s (4–8 producers), local DRAM ~16 GB/s
  (8 producers). The modeled side composes linearly; the physical side
  saturates at the calibrated bandwidth — both curves in
  `scaling_graph.pdf` (Fig. 5 of the IPDPS27 paper, §IV-D).

## Motivation experiment: sharing is the killer app for Type-2

Per-update cost of delivering a K-byte object update, charged by the device
accounting (`qtest_sharing_motivation.py`; bulk HTOD/DTOH paths now charge
media-write/read rails, exposed as TIMING_GET data slots 8–9):

| Strategy | Cost per update | vs. sharing |
| --- | --- | --- |
| Coherent sharing (doorbell + on-demand cache read) | **371 ns, flat in K** | 1× |
| Explicit publication (push K B + doorbell + read) | 622 ns @64B → 1.03 ms @256KiB | 1.7× → **2775×** |
| Full replication (256 KiB structure) | 1.03 ms flat | **2775×** |

The advantage is a slope, not a constant: sharing converts
push-through-the-link into pull-on-demand, and the conversion compounds with
object size (44× at 4 KiB). Together with the physical batching result
(647 → 9.2 ns per notification, 70×), these are the two motivated regimes —
both figures and text are in the IPDPS27 paper (§IV-D, Fig. 4 of
`splash_ipdps27.tex`).

## Timing model patch (dead fields → wired)

The previously dead `latency_enabled/read_latency_ns/write_latency_ns/
coherency_latency_ns` fields are now live:

- **Properties**: `latency-enabled` (default off — functional tests unchanged),
  `read-latency-ns` (120, device-cache hit rail = paper HMC hit),
  `write-latency-ns` (250, HDM media write rail),
  `coherency-latency-ns` (112, H2D notification/snoop round trip, also charged
  on miss fills), `bandwidth-gbps` (51, serialized link ceiling),
  `hmc-install-ns` (8, DCOH fill serialization — one install per device-cache
  fill bounds the allocating fill rate at 64 B/8 ns = 8.0 GB/s vs the 51 GB/s
  link, the paper's 6.2× structure), `allocate-on-miss` (true default;
  false = non-allocating RdCurr class: no fill, no install, repeats still miss).
- **Accounting, not stalling**: modeled time accumulates per access
  (device-side ns) and never delays the guest — CXLMemSim's model-time
  philosophy. Read path: hit/miss rails; write path: media rail + fill;
  serialized link time `bytes / bandwidth` added to both; bulk HTOD/DTOH
  commands charge per-64B-line rails; installs counted separately so rail
  checks stay exact.
- **New commands**: `0xF0 NOTIFY_BATCH` (params: base, count, batch size,
  payload — models each notification as an HDM media write and each batch as
  one doorbell/GO round trip), `0xF1 TIMING_GET` (results[0..3] = total ns,
  hits, misses, notifications; data buffer slots 0–11 = per-class breakdown
  incl. bulk and installs), `0xF2 TIMING_RESET`.
- **Verified** (`qtest_notify_smoke.py`, 19 exact checks): hit = 120 ns,
  miss = 232 ns, write = 250 ns, miss+install = 240 ns (one install per fill,
  8 ns), bulk HTOD 640 B = 2512 ns, bulk DTOH = 1212 ns, notify total =
  count·(250+⌊64/51⌋) + ⌈count/batch⌉·112, non-allocating = 0 installs and
  still-miss on re-read — all exact. `qtest_type2_paper.py` functional
  suite: 23/23 unchanged.
- **Full suite**: `./run_full_suite.sh` runs functional + smoke + notify +
  sharing + DCOH + scaling (1/4/32) and prints a summary; current status
  **SUITE: ALL PASS**.

## Notification batch-size microbench

Producer posting notifications (64 B events) with one doorbell per batch of B:

- **Simulated** (`qtest_notify_bench.py`, device accounting): per-notification
  latency 363 ns (B=1) → 251 ns (B≥128); single-stream 0.18 → 0.255 GB/s
  (media-write-bound; the paper's 665 GB/s HDM ceiling needs device-internal
  parallelism a synchronous single stream doesn't generate).
- **Real HW** (`/root/Splash/bench/cxl_notify_bench/`, this Threadripper +
  Montage node): roundtrip mode (NT store + fence + read-back) drops
  **647 → 9.2 ns** per notification from B=1→1024 on the CXL node vs
  **328 → 7.9 ns** on local DDR5 (2.0× CXL/DRAM ratio unbatched); doorbell
  (same-line version bump) coalesces to 0.1 ns; NT push settles at ~7 GB/s
  single-stream. `notify_hw.csv` holds the full matrix.

## Calibration graph

`calibration_graph.png` (regenerate: `python3 make_calibration_graph.py`):
(A) per-notification latency vs batch size — device model ≡ paper stage-cost
model (coincident curves = the calibration result) against real-HW CXL/DRAM/doorbell
curves; (B) single-stream throughput vs batch with the 51 GB/s paper link
ceiling; (C) rail calibration check — device model 120/232/250 ns vs paper values.

## Parameter mapping (paper → CXLMemSim)

### Application-level CXLMemSim (`run_calibrated_legacy.sh`)

| Parameter | Value | Paper source | Status |
| --- | --- | --- | --- |
| `-d` DRAM latency | 103 ns | §4.1 IntraC DRAM rail | reported |
| `-f` frequency | 4000 MHz | §4.1 core pinning | reported |
| `-o` topology | `(1)` | single CXL endpoint = the Type-2 device's HDM (topology numbers are 1-indexed expanders, root implicit). 2-endpoint variant: `(1,2)` with expander 2 = remote-socket DRAM at the InterC rails 165 ns / 96 GB/s | reported (layout) |
| `-l` expander read/write latency | 220 / 250 ns | derived: 25 (issue) + 22 (TOR) + 45 (CXL) + ~80 (DMC/media) + 45 (return) ≈ 217 → 220; write +30 (media fill) | derived |
| `-b` expander read/write BW | 51 / 51 GB/s | §4.4: 51.0 GB/s = 99% of CXL flit rate, both directions | reported |
| `--mlc-bandwidth` | 51,51,46 | read/write reported; mixed = 51×0.9 heuristic | derived |
| `--bandwidth-knee` | 0.80 (default) | not reported; use 0.43 to reproduce host-DRAM queueing (59/137.4 GB/s knee, Fig 7d) | cxlmemsim_default |
| `-q` capacity | 256,32 GB | HDM size not reported; adjust to actual board | cxlmemsim_default |

### QEMU `cxl-type2` device (`launch_qemu_type2_calibrated.sh`)

| Property | Value | Paper source | Status |
| --- | --- | --- | --- |
| `cache-size` | **256K** | HMC = 256 KiB (§4.1); default 128M is 512× too large and would erase the HMC-miss regime (Fig 6a knee at 3.5 MiB/core) | reported |
| `x-speed`/`x-width`/`x-256b-flit` | 32 / 16 / off | PCIe 5.0 x16, CXL 2.0 (68B flits) | reported |
| `hdm-db` | false | HDM has no device-side directory (§2.1); model also requires this with 68B flits | reported |
| `gfam-latency-ns` | 170 | CXL crossing 45 + completion 125 (Fig 5c stages) | derived |
| `gfam-bandwidth-mbps` | 52224 | 51 GB/s ceiling; property unit is MiB/s (default 32768 = 32 GiB/s) | reported (converted) |
| `mhsld-coh-latency-ns` | 112 | H2D snoop round trip 45+22+45 (Fig 5c stages); cf. reported 79 ns stale-snoop premium | derived |

### Coherency stage costs (for `cxl_type2_coherency.c` / `coherency_engine.cpp` work)

| Cost | Value | Paper source |
| --- | --- | --- |
| HMC hit | 120 ns | §4.1 |
| HMC fill | 30 ns | Fig 5c |
| Device completion | 125 ns | Fig 5c |
| H2D snoop round trip | 112 ns | derived (45+22+45) |
| **HMC install backpressure** | **1 per 7.9 ns** | §4.4: RdShared ceiling 8.16 GB/s vs RdCurr 51 GB/s = the paper's headline 6.2× allocating/non-allocating gap |
| RSF stale-entry snoop | 79 ns | §A.3 |
| First-touch install premium | 75 ns | §A.3 |

## What the current code cannot express (gaps)

1. ~~**Dead latency fields.**~~ **RESOLVED** — see "Timing model patch" above.
   Remaining nuance: the rails are *accounting* (model time), not guest-visible
   stalls; full-system guest-visible slowdown would need timer-based command
   completion if ever required.
2. **No allocation backpressure.** The 6.2× allocating-vs-non-allocating gap
   (DCOH 7.9 ns/HMC-install ceiling) has no knob anywhere in the device or
   fabric model. This is the paper's most important Type-2 behavior; model it as
   a per-install service period on the device-cache allocation path.
3. **No host-coherence substrate.** Per-CHA saturation (Alloc 2.7 vs Lookup
   12.46 GB/s), the RSF 16 MiB capacity cliff (665 → 39.5 GB/s), HitMe warm/cold
   behavior, and TOR residency are all measured in
   `paper_measurements.json` (`cha_pressure`, `coherency_structures`) but have
   no counterpart in the switch/expander model. They are ready to be wired when
   those experiments need reproducing.
4. **Host→HDM latency.** Fig 13(a) plots LS→HDM / RS→HDM but the values are
   figure-only (not in the text layer); the 220/250 ns values are the derived
   stage sum. Replace with digitized readouts. The real-host notification bench
   adds an anchor: CXL roundtrip 647 ns/notification unbatched on this node.

## Validation (C2 Sight §3.7 Stage-1 style rails check)

C2 Sight validates runs against calibrated reference rails (private-cache hits,
local-memory accesses). The CXLMemSim analog: run the pointer-chasing microbench
under the calibrated simulator and check the predicted rails against the paper:

```bash
# build microbenches and simulator
cmake -B build -S . && cmake --build build -j --target cache-miss CXLMemSim

# calibrated run (rails to check in the output):
#   local DRAM 103 ns, HDM ~220/250 ns, CXL link ceiling 51 GB/s
./calibration/c2sight_type2/run_calibrated_legacy.sh --target build/microbench/cache-miss
```

Acceptance: predicted local-DRAM-path latency within ±10% of 103 ns; HDM-path
latency within ±15% of 220 ns (the ±15% follows the paper's own inferred-path
matches, "within 7% for all cases and within 1% for three of four device
operations", §4.2 — loosened for the derived host→HDM input). Runs outside the
rails are discarded, per §3.7.

The QEMU side: `qemu_integration/qtest_type2_paper.py` remains the functional
check (its manifest correctly states qtest wall time is transport-bound, not
CXL timing); latency validation needs gap 1 fixed first.

## Refining this calibration on real hardware

The methodology the paper used to produce these numbers is reusable on any
testbed (this Threadripper + Montage host included, modulo Intel-CHA-specific
parts): latency/BW rails via working-set sweep (Fig 4b), counter noise floors
via matched idle windows, capacity knees via victim-retention sweeps (§A.3).
`paper_measurements.json` records which experiment produced which number so the
same experiment run locally can overwrite the corresponding entry.
