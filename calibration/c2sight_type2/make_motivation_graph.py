#!/usr/bin/env python3
"""Motivation figure: notification batching + coherent Type-2 sharing.

Panel A  physical per-notification cost vs batch size (real host) with the
         calibrated device model for reference -- batching alone recovers 70x.
Panel B  modeled per-update cost of delivering a K-byte object update:
         coherent sharing (flat) vs explicit publication (linear in K) vs
         full replication of a 256 KiB structure.

Data: notify_bench_results/{notify_hw.csv, notify_bench.json,
      sharing_motivation.csv}.  Palette: dataviz reference categorical order.
"""
from __future__ import annotations

import csv
import json
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
C_REPL = "#4a3aa7"
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

hw = defaultdict(lambda: defaultdict(list))
with (DATA / "notify_hw.csv").open() as f:
    for row in csv.DictReader(f):
        hw[(int(row["node"]), row["mode"])][int(row["batch"])].append(
            float(row["per_notify_ns"]))


def med(d, b):
    v = sorted(d[b])
    return v[len(v) // 2]


sim = json.loads((DATA / "notify_bench.json").read_text())
sim_batch = sorted(int(k) for k in sim["summary"])
sim_per = [sim["summary"][str(b)]["modeled_per_notify_ns"] for b in sim_batch]

shares = []
with (DATA / "sharing_motivation.csv").open() as f:
    for row in csv.DictReader(f):
        shares.append({k: float(v) for k, v in row.items()})
kvals = [r["object_bytes"] for r in shares]

fig, (axa, axb) = plt.subplots(1, 2, figsize=(10.5, 4.3))
fig.patch.set_facecolor(SURFACE)
fig.subplots_adjust(left=0.07, right=0.97, top=0.72, bottom=0.15, wspace=0.27)

# ---- Panel A: batching ---------------------------------------------------
for ax in (axa, axb):
    ax.set_facecolor(SURFACE)

rt1 = sorted(hw[(1, "roundtrip")])
rt0 = sorted(hw[(0, "roundtrip")])
db = sorted(hw[(1, "doorbell")])
axa.plot(rt1, [med(hw[(1, "roundtrip")], b) for b in rt1], "-s",
         color=C_CXL, lw=1.8, ms=4.5, label="HW CXL node, roundtrip")
axa.plot(rt0, [med(hw[(0, "roundtrip")], b) for b in rt0], "-s",
         color=C_DRAM, lw=1.8, ms=4.5, label="HW local DRAM, roundtrip")
axa.plot(db, [med(hw[(1, "doorbell")], b) for b in db], "-^",
         color=C_DOORBELL, lw=1.8, ms=4.5, label="HW CXL node, doorbell")
axa.plot(sim_batch, sim_per, "--", color=C_MODEL, lw=1.8,
         label="Calibrated device model")
axa.set_xscale("log", base=2)
axa.set_yscale("log")
axa.set_xlabel("Notifications per batch")
axa.set_ylabel("Per-notification cost (ns)")
axa.set_title("(A)  Batching: 70x on the notification path",
              loc="left", fontsize=10.5, color=TEXT, fontweight="bold")
axa.legend(loc="lower left", fontsize=7.5)
axa.annotate("70x", (64, 40), fontsize=11, color=C_CXL, fontweight="bold")
axa.annotate("647 ns", (1, 647), xytext=(9, -3), textcoords="offset points",
             fontsize=7.5, color=C_CXL)
axa.annotate("9.2 ns", (1024, 9.2), xytext=(-2, 8), textcoords="offset points",
             ha="right", fontsize=7.5, color=C_CXL)

# ---- Panel B: sharing ------------------------------------------------------
axb.plot(kvals, [r["share_ns_per_update"] for r in shares], "-o",
         color=C_MODEL, lw=1.8, ms=4.5, label="Coherent sharing (this work)")
axb.plot(kvals, [r["publish_ns_per_update"] for r in shares], "-s",
         color=C_CXL, lw=1.8, ms=4.5, label="Explicit publication (push K B)")
axb.plot(kvals, [r["replicate_ns_per_update"] for r in shares], "--",
         color=C_REPL, lw=1.8, label="Full replication (256 KiB structure)")
axb.set_xscale("log", base=2)
axb.set_yscale("log")
axb.set_xlabel("Updated object size (bytes)")
axb.set_ylabel("Modeled cost per update (ns)")
axb.set_title("(B)  Sharing vs. publication vs. replication",
              loc="left", fontsize=10.5, color=TEXT, fontweight="bold")
axb.legend(loc="center left", fontsize=7.5)
axb.annotate("371 ns, flat", (64, 371), xytext=(4, 6), textcoords="offset points",
             fontsize=7.5, color=C_MODEL)
axb.annotate("44x @ 4 KiB", (4096, 16451), xytext=(6, -13),
             textcoords="offset points", ha="left", fontsize=7.5, color=C_CXL)
axb.annotate("2775x @ 256 KiB", (262144, 1029511), xytext=(-4, -14),
             textcoords="offset points", ha="right", fontsize=7.5, color=C_REPL)

fig.suptitle("Two workload properties that motivate CXL Type-2 coherent sharing",
             x=0.07, y=0.975, ha="left", fontsize=12, color=TEXT,
             fontweight="bold")
fig.text(0.07, 0.925,
         "(A) Physical host: producer posting 64 B notifications to CXL-attached memory; one fence+readback per batch.  "
         "(B) Calibrated device model:\n"
         "sharing = one doorbell write + one on-demand device-cache read; publication = push the full object through the link, then doorbell + read.",
         fontsize=7.5, color=TEXT2, va="top")

fig.savefig(HERE / "motivation_graph.png", dpi=160, facecolor=SURFACE)
fig.savefig(HERE / "motivation_graph.pdf", facecolor=SURFACE)
print(f"wrote {HERE / 'motivation_graph.pdf'}")
