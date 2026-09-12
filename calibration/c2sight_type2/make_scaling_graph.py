#!/usr/bin/env python3
"""Scaling figure: platform composition to 32 endpoints + physical saturation.

Panel A  modeled aggregate throughput vs device count (1..32 Type-2 endpoints
         in one QEMU): notification stream and coherent-sharing updates —
         the accounting composes linearly (100% efficiency).
Panel B  physical producer-parallelism scaling on the calibration host:
         aggregate roundtrip throughput vs producer thread count; the CXL
         device saturates near 11.5 GB/s, local DRAM near 16 GB/s.

Data: notify_bench_results/{scale_bench.csv, threads_hw.csv}.
"""
from __future__ import annotations

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
DATA = HERE / "notify_bench_results"

C_MODEL = "#2a78d6"
C_DOORBELL = "#1baf7a"
C_DRAM = "#eda100"
C_CXL = "#e34948"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e5e4e0"
SURFACE = "#fcfcfb"

plt.rcParams.update({
    "font.size": 9, "text.color": TEXT,
    "axes.edgecolor": "#c9c8c4", "axes.labelcolor": TEXT2,
    "xtick.color": TEXT2, "ytick.color": TEXT2,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.7,
    "axes.axisbelow": True, "legend.frameon": False,
})

rows = list(csv.DictReader((DATA / "scale_bench.csv").open()))
ns = sorted({int(r["devices"]) for r in rows})
notify = {int(r["devices"]): float(r["agg_modeled_gbps"]) for r in rows
          if r["experiment"] == "notify"}
share_eff = {int(r["devices"]): float(r["scaling_efficiency"]) for r in rows
             if r["experiment"] == "share"}
share_agg = {int(r["devices"]): 64.0 * 64 / float(r["per_device_ns"]) * int(r["devices"])
             for r in rows if r["experiment"] == "share"}

th = defaultdict(lambda: defaultdict(list))
with (DATA / "threads_hw.csv").open() as f:
    for row in csv.DictReader(f):
        th[int(row["node"])][int(row["threads"])].append(
            float(row["agg_gbps"]))


def med(d, k):
    v = sorted(d[k])
    return v[len(v) // 2]


fig, (axa, axb) = plt.subplots(1, 2, figsize=(10.5, 3.9))
fig.patch.set_facecolor(SURFACE)
fig.subplots_adjust(left=0.07, right=0.97, top=0.74, bottom=0.16, wspace=0.27)
for ax in (axa, axb):
    ax.set_facecolor(SURFACE)

# ---- Panel A: modeled device-count scaling -------------------------------
not0 = notify[1]
sh0 = share_agg[1]
axa.plot(ns, [notify[n] / not0 for n in ns], "-o", color=C_MODEL, lw=1.8,
         ms=4.5, label="Notification stream (batch 64)")
axa.plot(ns, [share_agg[n] / sh0 for n in ns], "-s", color=C_DOORBELL,
         lw=1.8, ms=4.5, label="Coherent-sharing update rate")
axa.plot(ns, ns, ":", color=TEXT2, lw=1.2, label="Perfect linearity (1:1)")
axa.set_xscale("log", base=2)
axa.set_yscale("log", base=2)
axa.set_xlabel("Type-2 endpoints in one QEMU instance")
axa.set_ylabel("Aggregate speedup over 1 endpoint")
axa.set_title("(A)  Device-count scaling: 100% efficient to 32 endpoints",
              loc="left", fontsize=10, color=TEXT, fontweight="bold")
axa.legend(loc="upper left", fontsize=7.5)
axa.annotate("32 endpoints: 8.1 GB/s notifications,\n86M shared updates/s",
             (32, 32), xytext=(0, -26), textcoords="offset points",
             ha="right", fontsize=7.5, color=C_MODEL)

# ---- Panel B: physical producer scaling ----------------------------------
t1 = sorted(th[1])
t0 = sorted(th[0])
axb.plot(t1, [med(th[1], k) for k in t1], "-s", color=C_CXL, lw=1.8, ms=4.5,
         label="HW CXL node")
axb.plot(t0, [med(th[0], k) for k in t0], "-s", color=C_DRAM, lw=1.8, ms=4.5,
         label="HW local DRAM")
axb.axhline(11.5, color=C_CXL, ls="--", lw=1.2)
axb.axhline(16.0, color="#b07800", ls="--", lw=1.2)
axb.annotate("CXL device saturates ~11.5 GB/s", (32, 11.5), xytext=(-2, 5),
             textcoords="offset points", ha="right", fontsize=7.5, color=C_CXL)
axb.annotate("DRAM ~16 GB/s", (32, 16.2), xytext=(-2, 4),
             textcoords="offset points", ha="right", fontsize=7.5, color="#b07800")
axb.set_xscale("log", base=2)
axb.set_xlabel("Producer threads (physical device, batch 64)")
axb.set_ylabel("Aggregate roundtrip throughput (GB/s)")
axb.set_title("(B)  Physical scaling saturates at the device",
              loc="left", fontsize=10, color=TEXT, fontweight="bold")
axb.legend(loc="lower right", fontsize=7.5)

fig.suptitle("Scaling: the platform composes linearly; calibrated rails bound the physical ceiling",
             x=0.07, y=0.96, ha="left", fontsize=12, color=TEXT,
             fontweight="bold")
fig.text(0.07, 0.885,
         "(A) Modeled service time summed over independent endpoints, one QEMU instance (qtest).  "
         "(B) Physical host: roundtrip mode, one fence+readback per 64-notification batch per producer.",
         fontsize=7.5, color=TEXT2, va="top")

fig.savefig(HERE / "scaling_graph.png", dpi=160, facecolor=SURFACE)
fig.savefig(HERE / "scaling_graph.pdf", facecolor=SURFACE)
print(f"wrote {HERE / 'scaling_graph.pdf'}")
