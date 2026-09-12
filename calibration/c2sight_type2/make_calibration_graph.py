#!/usr/bin/env python3
"""CXL Type-2 calibration figure for the C2 Sight calibration report.

Three panels, one axis each:
  A  per-notification latency vs batch size (log-log):
     device model (qtest accounting) vs real HW (Threadripper + Montage CXL
     node / local DRAM roundtrip mode) vs the paper's stage-cost model.
  B  single-stream throughput vs batch size (GB/s).
  C  device-model rails vs paper values (the calibration check itself).

Data: notify_bench_results/notify_bench.json (simulated) and
      notify_bench_results/notify_hw.csv (real HW).

Palette: dataviz reference categorical order, validated (see README).
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

# Validated categorical slots (fixed order, assigned by entity).
C_MODEL = "#2a78d6"    # blue  — simulated device model
C_DOORBELL = "#1baf7a"  # aqua  — HW doorbell mode (relief: direct labels)
C_DRAM = "#eda100"     # yellow — HW local DRAM (relief: direct labels)
C_PAPER = "#4a3aa7"    # violet — paper-reported values
C_CXL = "#e34948"      # red   — HW CXL node
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e5e4e0"
SURFACE = "#fcfcfb"

plt.rcParams.update({
    "font.size": 9,
    "text.color": TEXT,
    "axes.edgecolor": "#c9c8c4",
    "axes.labelcolor": TEXT2,
    "xtick.color": TEXT2,
    "ytick.color": TEXT2,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.7,
    "axes.axisbelow": True,
    "legend.frameon": False,
})

# ---- simulated device model -------------------------------------------
sim = json.loads((DATA / "notify_bench.json").read_text())
sim_batch = sorted(int(k) for k in sim["summary"])
sim_per = [sim["summary"][str(b)]["modeled_per_notify_ns"] for b in sim_batch]
sim_gbps = [sim["summary"][str(b)]["modeled_stream_gbps"] for b in sim_batch]

# paper stage-cost model: per-notify = write(250) + link; per-batch +112/B
paper_batch = sim_batch
paper_per = [250 + 112 / b for b in paper_batch]

# ---- real HW -----------------------------------------------------------
hw = defaultdict(lambda: defaultdict(list))  # (node, mode) -> batch -> [per_ns]
hw_gbps = defaultdict(lambda: defaultdict(list))
with (DATA / "notify_hw.csv").open() as f:
    for row in csv.DictReader(f):
        key = (int(row["node"]), row["mode"])
        b = int(row["batch"])
        hw[key][b].append(float(row["per_notify_ns"]))
        hw_gbps[key][b].append(float(row["stream_gbps"]))


def med(d, b):
    vals = sorted(d[b])
    return vals[len(vals) // 2]


fig = plt.figure(figsize=(10.5, 7.2), facecolor=SURFACE)
gs = fig.add_gridspec(2, 2, height_ratios=[1.45, 1.0], hspace=0.42, wspace=0.28,
                      left=0.075, right=0.97, top=0.86, bottom=0.10)

# ---- Panel A: latency vs batch -----------------------------------------
ax = fig.add_subplot(gs[0, :])
ax.set_facecolor(SURFACE)
ax.plot(sim_batch, sim_per, "-o", color=C_MODEL, lw=1.8, ms=4.5,
        label="Device model (qtest, calibrated)")
ax.plot(paper_batch, paper_per, "--", color=C_PAPER, lw=1.8,
        label="Paper stage-cost model (250 + 112/B)")
ax.plot(sorted(hw[(1, "roundtrip")]), [med(hw[(1, "roundtrip")], b) for b in sorted(hw[(1, "roundtrip")])],
        "-s", color=C_CXL, lw=1.8, ms=4.5, label="HW: CXL node, roundtrip")
ax.plot(sorted(hw[(0, "roundtrip")]), [med(hw[(0, "roundtrip")], b) for b in sorted(hw[(0, "roundtrip")])],
        "-s", color=C_DRAM, lw=1.8, ms=4.5, label="HW: local DRAM, roundtrip")
ax.plot(sorted(hw[(1, "doorbell")]), [med(hw[(1, "doorbell")], b) for b in sorted(hw[(1, "doorbell")])],
        "-^", color=C_DOORBELL, lw=1.8, ms=4.5, label="HW: CXL node, doorbell")
ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel("Notifications per batch")
ax.set_ylabel("Per-notification latency (ns)")
ax.set_title("(A)  Notification latency vs. batch size", loc="left", fontsize=10.5,
             color=TEXT, fontweight="bold")
ax.legend(loc="lower right", bbox_to_anchor=(0.98, 0.16), fontsize=8)
# direct labels at line ends (relief rule for aqua/yellow); the model and
# paper curves coincide -- that agreement IS the calibration result -- so
# their end labels are split above/below the shared line.
ax.annotate("paper\nmodel", (1024, paper_per[-1]), xytext=(6, 14),
            textcoords="offset points", fontsize=7.5, color=C_PAPER)
ax.annotate("device\nmodel", (1024, sim_per[-1]), xytext=(6, -22),
            textcoords="offset points", fontsize=7.5, color=C_MODEL)
ax.annotate("CXL\nroundtrip", (1024, med(hw[(1, "roundtrip")], 1024)), xytext=(6, 6),
            textcoords="offset points", fontsize=7.5, color=C_CXL)
ax.annotate("DRAM", (1024, med(hw[(0, "roundtrip")], 1024)), xytext=(6, -14),
            textcoords="offset points", fontsize=7.5, color="#b07800")
ax.annotate("doorbell", (1024, med(hw[(1, "doorbell")], 1024)), xytext=(6, -4),
            textcoords="offset points", fontsize=7.5, color=C_DOORBELL)

# ---- Panel B: throughput vs batch ---------------------------------------
axb = fig.add_subplot(gs[1, 0])
axb.set_facecolor(SURFACE)
axb.plot(sim_batch, sim_gbps, "-o", color=C_MODEL, lw=1.8, ms=4.5,
         label="Device model")
rt1 = sorted(hw[(1, "roundtrip")])
rt0 = sorted(hw[(0, "roundtrip")])
axb.plot(rt1, [med(hw_gbps[(1, "roundtrip")], b) for b in rt1], "-s",
         color=C_CXL, lw=1.8, ms=4.5, label="HW CXL roundtrip")
axb.plot(rt0, [med(hw_gbps[(0, "roundtrip")], b) for b in rt0], "-s",
         color=C_DRAM, lw=1.8, ms=4.5, label="HW DRAM roundtrip")
axb.axhline(51, color=C_PAPER, ls="--", lw=1.4)
axb.annotate("paper CXL link ceiling 51 GB/s", (1, 51), xytext=(4, 5),
             textcoords="offset points", fontsize=7.5, color=C_PAPER)
# series share panel A's colors; direct labels instead of a second legend
axb.annotate("device model", (1024, sim_gbps[-1]), xytext=(0, -12),
             textcoords="offset points", ha="right", fontsize=7.5, color=C_MODEL)
axb.annotate("CXL", (1024, med(hw_gbps[(1, "roundtrip")], 1024)), xytext=(0, -14),
             textcoords="offset points", ha="right", fontsize=7.5, color=C_CXL)
axb.annotate("DRAM", (1024, med(hw_gbps[(0, "roundtrip")], 1024)), xytext=(0, 7),
             textcoords="offset points", ha="right", fontsize=7.5, color="#b07800")
axb.set_xscale("log", base=2)
axb.set_yscale("log")
axb.set_ylim(0.07, 150)
axb.set_xlabel("Notifications per batch")
axb.set_ylabel("Single-stream throughput (GB/s)")
axb.set_title("(B)  Single-stream throughput", loc="left", fontsize=10.5,
              color=TEXT, fontweight="bold")

# ---- Panel C: rail calibration check ------------------------------------
axc = fig.add_subplot(gs[1, 1])
axc.set_facecolor(SURFACE)
rails = ["hit\n(120)", "miss\n(120+112)", "miss+install\n(232+8)", "write\n(250)"]
measured = [sim["rails"]["hit_ns"], sim["rails"]["miss_ns"], 240,
            sim["rails"]["write_ns"]]
paper_r = [120, 232, 240, 250]
x = range(len(rails))
w = 0.36
axc.bar([i - w / 2 for i in x], paper_r, w, color=C_PAPER, label="Paper value",
        edgecolor=SURFACE, linewidth=2)
axc.bar([i + w / 2 for i in x], measured, w, color=C_MODEL, label="Device model",
        edgecolor=SURFACE, linewidth=2)
for i, v in enumerate(paper_r):
    axc.annotate(f"{v}", (i - w / 2, v), xytext=(0, 3), textcoords="offset points",
                 ha="center", fontsize=7.5, color=TEXT2)
for i, v in enumerate(measured):
    axc.annotate(f"{v}", (i + w / 2, v), xytext=(0, 3), textcoords="offset points",
                 ha="center", fontsize=7.5, color=TEXT2)
axc.set_xticks(list(x), rails, fontsize=7)
axc.set_ylabel("Latency (ns)")
axc.set_ylim(0, 285)
axc.grid(axis="x", visible=False)
axc.set_title("(C)  Rail calibration check", loc="left", fontsize=10.5,
              color=TEXT, fontweight="bold")
axc.legend(loc="upper left", fontsize=8)

fig.suptitle("CXL Type-2 calibration — device model vs. characterization rails vs. real host",
             x=0.075, y=0.965, ha="left", fontsize=12, color=TEXT, fontweight="bold")
fig.text(0.075, 0.925,
         "Device: patched cxl-type2 QEMU model, rails 120/250/112 ns, 51 GB/s  ·  "
         "HW: Threadripper 7960X + Montage CXL node (node1) vs local DDR5 (node0), 64 B events\n"
         "Model semantics: media write + one doorbell/GO per batch;  HW roundtrip: NT store + sfence + read-back  ·  "
         "median of repeated runs",
         fontsize=7.5, color=TEXT2, va="top")

out = HERE / "calibration_graph.png"
fig.savefig(out, dpi=160, facecolor=SURFACE)
fig.savefig(HERE / "calibration_graph.pdf", facecolor=SURFACE)
print(f"wrote {out}")
