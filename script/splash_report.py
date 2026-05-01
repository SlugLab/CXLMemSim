#!/usr/bin/env python3
"""
Splash report generator.

Reads the CSVs produced by splash_aggregate.py and emits:
  - Markdown summary (splash_summary.md)
  - One log-log PDF per RQ (matplotlib)

Runs standalone; does not require the VM.
"""
from __future__ import annotations

import argparse
import csv
import pathlib
import statistics
import sys
from collections import defaultdict

ARTIFACT = pathlib.Path("/home/victoryang00/CXLMemSim/artifact/splash_sweep")


def read_csv(path):
    if not path.exists():
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def summarize_rq1_bfs(rows):
    """Group by (graph, N, device_fraction, method); report median_us stats."""
    out = []
    seen_keys = set()
    for r in rows:
        k = (r["graph"], r["N"], r["device_fraction"], r["method"])
        if k in seen_keys: continue
        seen_keys.add(k)
        out.append({
            "graph": r["graph"], "N": r["N"], "device_fraction": r["device_fraction"],
            "method": r["method"], "method_name": r.get("method_name",""),
            "median_us": r["median_us"], "bias_flips": r.get("bias_flips",""),
            "dir_entries": r.get("dir_entries",""),
        })
    return out


def write_markdown(outdir, csv_dir):
    md = ["# Splash Sweep Results", ""]

    bfs = read_csv(csv_dir / "rq1_bfs.csv")
    md.append("## RQ1: Pointer-sharing vs. copy-based BFS")
    if not bfs:
        md.append("_No rq1_bfs rows._")
    else:
        md.append(f"Rows: {len(bfs)}")
        md.append("")
        md.append("| graph | N | dev_frac | method | method_name | median_us | bias_flips | dir_entries |")
        md.append("|---|---|---|---|---|---|---|---|")
        for r in sorted(bfs, key=lambda x: (x.get("graph",""), int(x.get("N") or 0),
                                            float(x.get("device_fraction") or 0),
                                            x.get("method",""))):
            md.append("| {} | {} | {} | {} | {} | {} | {} | {} |".format(
                r.get("graph",""), r.get("N",""), r.get("device_fraction",""),
                r.get("method",""), r.get("method_name",""),
                r.get("median_us",""), r.get("bias_flips",""), r.get("dir_entries","")))
    md.append("")

    btree = read_csv(csv_dir / "rq1_btree.csv")
    md.append("## RQ1b: B+ tree lookup")
    if btree:
        md.append("| B | num_leaves | method | median_us | p25_us | p75_us |")
        md.append("|---|---|---|---|---|---|")
        for r in btree:
            md.append("| {} | {} | {} | {} | {} | {} |".format(
                r.get("B",""), r.get("num_leaves",""), r.get("method",""),
                r.get("median_us",""), r.get("p25_us",""), r.get("p75_us","")))
    else:
        md.append("_No data._")
    md.append("")

    hash_rows = read_csv(csv_dir / "rq1_hash.csv")
    md.append("## RQ1c: Hash table PUT/GET")
    if hash_rows:
        md.append("| op | median_us | p25_us | p75_us | log |")
        md.append("|---|---|---|---|---|")
        for r in hash_rows[:40]:
            md.append("| {} | {} | {} | {} | {} |".format(
                r.get("op",""), r.get("median_us",""),
                r.get("p25_us",""), r.get("p75_us",""), r.get("log","")))
    else:
        md.append("_No data._")
    md.append("")

    for rq, title in [("rq2_dir", "RQ2: Directory capacity sweep"),
                       ("rq3_bias", "RQ3: Bias mode × KV workload"),
                       ("rq4_alloc", "RQ4: Allocation policy"),
                       ("rq4_devfrac", "RQ4: Hash table device fraction")]:
        rows = read_csv(csv_dir / f"{rq}.csv")
        md.append(f"## {title}")
        if not rows:
            md.append("_No data._"); md.append(""); continue
        cols = list(rows[0].keys())
        md.append("| " + " | ".join(cols) + " |")
        md.append("|" + "---|" * len(cols))
        for r in rows[:80]:
            md.append("| " + " | ".join(str(r.get(c,"")) for c in cols) + " |")
        md.append("")

    outpath = outdir / "splash_summary.md"
    outpath.write_text("\n".join(md))
    print(f"[md] {outpath}  ({len(md)} lines)")


def try_plots(outdir, csv_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[plots] matplotlib not available; skipping")
        return

    bfs = read_csv(csv_dir / "rq1_bfs.csv")
    if bfs:
        # Group by (graph, device_fraction, method) -> N-vs-median scatter
        groups = defaultdict(list)
        for r in bfs:
            try:
                groups[(r["graph"], r["method"], float(r["device_fraction"]))
                       ].append((int(r["N"]), int(r["median_us"])))
            except (ValueError, KeyError):
                continue
        plt.figure(figsize=(9,6))
        for key, pts in sorted(groups.items()):
            pts.sort()
            xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
            if not xs: continue
            plt.plot(xs, ys, marker="o",
                     label=f"{key[0][:3]}/{key[1]}/df={key[2]:.2f}")
        plt.xscale("log"); plt.yscale("log")
        plt.xlabel("N (graph vertices)")
        plt.ylabel("BFS median latency (µs)")
        plt.title("RQ1 BFS: pointer-share (A) vs copy (B) across scales")
        plt.grid(True, which="both", alpha=0.3)
        plt.legend(fontsize=7, ncol=2)
        plt.tight_layout()
        p = outdir / "rq1_bfs_scaling.pdf"
        plt.savefig(p); plt.close()
        print(f"[plot] {p}")

    devfrac = read_csv(csv_dir / "rq4_devfrac.csv")
    if devfrac:
        xs, ymed, yp25, yp75 = [], [], [], []
        for r in sorted(devfrac, key=lambda x: float(x.get("device_fraction") or 0)):
            xs.append(float(r["device_fraction"]))
            ymed.append(int(r["median_ops_sec"]))
            yp25.append(int(r["p25_ops_sec"]))
            yp75.append(int(r["p75_ops_sec"]))
        plt.figure(figsize=(7,5))
        plt.fill_between(xs, yp25, yp75, alpha=0.2)
        plt.plot(xs, ymed, marker="o", linewidth=2)
        plt.xlabel("device fraction")
        plt.ylabel("hash ops/sec (median, with p25-p75 band)")
        plt.title("RQ4: Hash table throughput vs device fraction")
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        p = outdir / "rq4_devfrac.pdf"
        plt.savefig(p); plt.close()
        print(f"[plot] {p}")

    rq3 = read_csv(csv_dir / "rq3_bias.csv")
    if rq3:
        # KV zipf experiment from the main (non-multigpu) source
        plt.figure(figsize=(8,5))
        by_label = defaultdict(list)
        for r in rq3:
            if r.get("source") and r["source"] != "main": continue
            if "KV Store" not in r.get("experiment",""): continue
            if not r.get("theta"): continue
            try:
                by_label[r["label"].strip()].append(
                    (float(r["theta"]), float(r["median"])))
            except ValueError:
                continue
        for label, pts in sorted(by_label.items()):
            pts.sort()
            xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
            plt.plot(xs, ys, marker="o", label=label)
        plt.xlabel("zipf theta (0 = uniform, 1 = skewed)")
        plt.ylabel("KV ops/sec (median)")
        plt.title("RQ3: Bias mode vs Zipf skew")
        plt.grid(True, alpha=0.3)
        plt.legend(fontsize=8)
        plt.tight_layout()
        p = outdir / "rq3_bias_zipf.pdf"
        plt.savefig(p); plt.close()
        print(f"[plot] {p}")

        # Multi-GPU scaling: 1T Device-Bias ops/sec vs accelerator count
        mg = defaultdict(list)
        for r in rq3:
            if not r.get("accel_count"): continue
            try:
                n = int(r["accel_count"])
                lbl = r["label"].strip()
                if "1T " in lbl or "2T " in lbl:
                    mg[lbl].append((n, float(r["median"])))
            except (ValueError, KeyError): continue
        if mg:
            plt.figure(figsize=(8,5))
            for lbl, pts in sorted(mg.items()):
                pts.sort()
                xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
                plt.plot(xs, ys, marker="o", label=lbl)
            plt.xlabel("accelerator contexts attached to the VM")
            plt.ylabel("shared counter ops/sec (median)")
            plt.title("Multi-GPU scaling: bias mode × accelerator count")
            plt.grid(True, alpha=0.3)
            plt.legend(fontsize=7, ncol=2)
            plt.tight_layout()
            p = outdir / "multigpu_scaling.pdf"
            plt.savefig(p); plt.close()
            print(f"[plot] {p}")

    rq2 = read_csv(csv_dir / "rq2_dir.csv")
    if rq2:
        plt.figure(figsize=(8,5))
        by_sec = defaultdict(list)
        for r in rq2:
            try:
                by_sec[r["section"]].append((int(r["N"]), float(r["norm_tput"])))
            except (ValueError, KeyError): continue
        for sec, pts in sorted(by_sec.items()):
            pts.sort()
            xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
            plt.plot(xs, ys, marker="o", label=sec)
        plt.xscale("log"); plt.xlabel("N (working-set size)")
        plt.ylabel("normalized throughput (1.0 = smallest N)")
        plt.title("RQ2: Working-set-scaling throughput cliff")
        plt.grid(True, which="both", alpha=0.3); plt.legend()
        plt.tight_layout()
        p = outdir / "rq2_working_set.pdf"
        plt.savefig(p); plt.close()
        print(f"[plot] {p}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv-dir", default=str(ARTIFACT / "csv"))
    ap.add_argument("--outdir", default=str(ARTIFACT / "report"))
    args = ap.parse_args()

    csv_dir = pathlib.Path(args.csv_dir)
    outdir = pathlib.Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    write_markdown(outdir, csv_dir)
    try_plots(outdir, csv_dir)


if __name__ == "__main__":
    main()
