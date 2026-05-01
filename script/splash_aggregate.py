#!/usr/bin/env python3
"""
Splash aggregator.

Parses the per-run stdout logs produced by splash_sweep_driver.py into CSVs:
  rq1_bfs.csv         — BFS methods × device_fraction × graph_type × N
  rq1_btree.csv       — B+ tree methods × branching factor
  rq1_hash.csv        — Hash table methods
  rq2_dir.csv         — Directory sizing: working-set vs directory pressure
  rq3_bias.csv        — Bias mode × KV workload
  rq4_alloc.csv       — Allocation policy × migration
  rq4_devfrac.csv     — Hash table device fraction sweep

Regexes stay tolerant — log formats from the guest microbenchmarks are
printf-based, not CSV. If a format drifts, extend the pattern here rather than
re-running the sweep.
"""
from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys

ARTIFACT = pathlib.Path("/home/victoryang00/CXLMemSim/artifact/splash_sweep")

# RQ1 patterns -------------------------------------------------------------
RE_RQ1_CONFIG = re.compile(
    r"^\s*graph=(\S+)\s+N=(\d+)\s+device_fraction=([\d.]+)")
RE_RQ1_METHOD = re.compile(
    r"^\s*Method\s+([AB])\s+\(([^)]+)\):\s+median=(\d+)\s+us\s+p25=(\d+)\s+us\s+p75=(\d+)\s+us")
RE_COH1 = re.compile(
    r"coherency:\s+snoop_hits=(\d+)\s+snoop_misses=(\d+)\s+coh_reqs=(\d+)\s+back_inv=(\d+)")
RE_COH2 = re.compile(
    r"writebacks=(\d+)\s+evictions=(\d+)\s+bias_flips=(\d+)")
RE_COH3 = re.compile(
    r"dev_bias_hits=(\d+)\s+host_bias_hits=(\d+)\s+dir_entries=(\d+)")

RE_EXPERIMENT = re.compile(r"^\s*EXPERIMENT\s+(\d+):\s+(.*)$")
RE_BTREE_B = re.compile(r"^\s*B=(\d+)\s+num_leaves=(\d+)")
RE_BTREE_LINE = re.compile(
    r"^\s*(Pointer-sharing lookup|Copy-based lookup):\s+median=(\d+)\s+us\s+p25=(\d+)\s+us\s+p75=(\d+)\s+us")

RE_HASH_LINE = re.compile(
    r"^\s*(PUT|GET):\s+median=(\d+)\s+us\s+p25=(\d+)\s+us\s+p75=(\d+)\s+us")

# RQ3 patterns -------------------------------------------------------------
RE_RQ3_EXPERIMENT = re.compile(r"EXPERIMENT\s+(\d+):\s+(.*)$")
RE_RQ3_THETA = re.compile(r"^\s*---\s*theta\s*=\s*([\d.]+)\s*---")
# Label then >=2 spaces then p25/median/p75 tokens
RE_RQ3_LINE = re.compile(
    r"^\s*(\S.*?\S)\s{2,}p25=([\d.]+)\s+median=([\d.]+)\s+p75=([\d.]+)(?:\s+(\S+))?\s*$")
RE_RQ3_COH1 = re.compile(
    r"coherency:\s+snoop_hits=(\d+)\s+snoop_misses=(\d+)\s+bias_flips=(\d+)\s+writebacks=(\d+)")
RE_RQ3_COH2 = re.compile(
    r"dev_bias_hits=(\d+)\s+host_bias_hits=(\d+)\s+back_inv=(\d+)\s+dir_entries=(\d+)")

# RQ4-devfrac --------------------------------------------------------------
RE_RQ4DF = re.compile(r"^f=([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)")


def parse_rq1(path: pathlib.Path, rows_bfs, rows_btree, rows_hash):
    txt = path.read_text(errors="replace")
    cur_exp = None
    cur_cfg = {}
    cur_btree_B = None
    cur_btree_leaves = None
    pending_method = None

    coh = {k: None for k in ("snoop_hits snoop_misses coh_reqs back_inv "
                              "writebacks evictions bias_flips "
                              "dev_bias_hits host_bias_hits dir_entries").split()}

    def flush_bfs():
        if pending_method is None:
            return
        row = {**cur_cfg, **pending_method, **coh, "log": path.name}
        rows_bfs.append(row)

    for line in txt.splitlines():
        m = RE_EXPERIMENT.match(line)
        if m:
            cur_exp = int(m.group(1))
            cur_cfg = {}
            pending_method = None
            continue
        if cur_exp == 1:
            m = RE_RQ1_CONFIG.match(line)
            if m:
                if pending_method:
                    flush_bfs()
                    pending_method = None
                cur_cfg = {"graph": m.group(1), "N": int(m.group(2)),
                           "device_fraction": float(m.group(3))}
                for k in coh: coh[k] = None
                continue
            m = RE_RQ1_METHOD.match(line)
            if m:
                if pending_method:
                    flush_bfs()
                pending_method = {
                    "method": m.group(1),
                    "method_name": m.group(2),
                    "median_us": int(m.group(3)),
                    "p25_us": int(m.group(4)),
                    "p75_us": int(m.group(5)),
                }
                for k in coh: coh[k] = None
                continue
        if cur_exp == 2:
            m = RE_BTREE_B.match(line)
            if m:
                cur_btree_B = int(m.group(1))
                cur_btree_leaves = int(m.group(2))
                continue
            m = RE_BTREE_LINE.match(line)
            if m and cur_btree_B is not None:
                rows_btree.append({
                    "B": cur_btree_B, "num_leaves": cur_btree_leaves,
                    "method": m.group(1),
                    "median_us": int(m.group(2)),
                    "p25_us": int(m.group(3)),
                    "p75_us": int(m.group(4)),
                    "log": path.name,
                })
                continue
        if cur_exp == 3:
            m = RE_HASH_LINE.match(line)
            if m:
                rows_hash.append({
                    "op": m.group(1),
                    "median_us": int(m.group(2)),
                    "p25_us": int(m.group(3)),
                    "p75_us": int(m.group(4)),
                    "log": path.name,
                })
                continue
        # Coherency stats lines can belong to any experiment; attach to last pending method
        m = RE_COH1.search(line)
        if m and pending_method:
            coh["snoop_hits"] = int(m.group(1))
            coh["snoop_misses"] = int(m.group(2))
            coh["coh_reqs"] = int(m.group(3))
            coh["back_inv"] = int(m.group(4))
            continue
        m = RE_COH2.search(line)
        if m and pending_method:
            coh["writebacks"] = int(m.group(1))
            coh["evictions"] = int(m.group(2))
            coh["bias_flips"] = int(m.group(3))
            continue
        m = RE_COH3.search(line)
        if m and pending_method:
            coh["dev_bias_hits"] = int(m.group(1))
            coh["host_bias_hits"] = int(m.group(2))
            coh["dir_entries"] = int(m.group(3))
            flush_bfs()
            pending_method = None
            continue
    if pending_method:
        flush_bfs()


def parse_rq3(path: pathlib.Path, rows):
    # Detect multi-GPU scaling context from the path (n1/n2/n4/n8)
    accel_count = None
    for part in path.parts:
        if part.startswith("n") and part[1:].isdigit():
            accel_count = int(part[1:])
            break
    source = "multigpu_n%d" % accel_count if accel_count else "main"
    txt = path.read_text(errors="replace")
    theta = None
    experiment = None
    pending = None
    coh = {}

    def flush():
        nonlocal pending, coh
        if pending is not None:
            pending.update(coh)
            pending["accel_count"] = accel_count
            pending["source"] = source
            rows.append(pending)
        pending = None
        coh = {}

    for line in txt.splitlines():
        m = RE_RQ3_EXPERIMENT.search(line)
        if m:
            flush()
            experiment = f"{m.group(1)}:{m.group(2).strip()}"
            theta = None
            continue
        m = RE_RQ3_THETA.match(line)
        if m:
            flush()
            theta = float(m.group(1))
            continue
        m = RE_RQ3_LINE.match(line)
        if m:
            flush()
            pending = {
                "experiment": experiment or "",
                "theta": theta,
                "label": m.group(1).strip(),
                "p25": float(m.group(2)),
                "median": float(m.group(3)),
                "p75": float(m.group(4)),
                "unit": m.group(5) or "",
                "log": path.name,
            }
            continue
        m = RE_RQ3_COH1.search(line)
        if m and pending is not None:
            coh["snoop_hits"] = int(m.group(1))
            coh["snoop_misses"] = int(m.group(2))
            coh["bias_flips"] = int(m.group(3))
            coh["writebacks"] = int(m.group(4))
            continue
        m = RE_RQ3_COH2.search(line)
        if m and pending is not None:
            coh["dev_bias_hits"] = int(m.group(1))
            coh["host_bias_hits"] = int(m.group(2))
            coh["back_inv"] = int(m.group(3))
            coh["dir_entries"] = int(m.group(4))
            flush()
            continue
    flush()


def parse_rq4_devfrac(path: pathlib.Path, rows):
    """rq4_devfrac output columns: f=<fraction>  median_ops_s  p25_ops_s  p75_ops_s"""
    txt = path.read_text(errors="replace")
    for line in txt.splitlines():
        m = RE_RQ4DF.match(line)
        if m:
            rows.append({
                "device_fraction": float(m.group(1)),
                "median_ops_sec": int(m.group(2)),
                "p25_ops_sec": int(m.group(3)),
                "p75_ops_sec": int(m.group(4)),
                "log": path.name,
            })


def parse_rq4_alloc(path: pathlib.Path, rows):
    """Parses rq4_alloc_policy's tabular summaries:
      Part 1:  Policy | Time(ms) | Cross-Dom | Coh.Reqs | Evictions | Throughput(N/s)
      Part 2:  HT Policy | Time(ms) | Cross-Dom | Coh.Reqs
    Policy names contain spaces, so we look for a 5-column or 4-column float/int tail.
    """
    txt = path.read_text(errors="replace")
    lines = txt.splitlines()
    section = None  # 'bfs' | 'hash' | None
    # Match a line that ends with 5 numeric tokens (Part 1 table rows):
    re_bfs = re.compile(
        r"^(?P<policy>\S[^\d]*?)\s{2,}([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")
    # 4 numeric tokens trailing (Part 2 hash table rows): policy  time  cross  coh
    re_hash = re.compile(
        r"^(?P<policy>\S[^\d]*?)\s{2,}([\d.]+)\s+(\d+)\s+(\d+)\s*$")
    for line in lines:
        if "Part 1 Summary" in line or "Part 1:" in line:
            section = "bfs"; continue
        if "Part 2:" in line or "Hash Table Sub" in line:
            section = "hash"; continue
        if section == "bfs":
            m = re_bfs.match(line)
            if m:
                rows.append({
                    "section": "bfs",
                    "policy": m.group("policy").strip(),
                    "time_ms": float(m.group(2)),
                    "cross_dom": int(m.group(3)),
                    "coh_reqs": int(m.group(4)),
                    "evictions": int(m.group(5)),
                    "throughput": int(m.group(6)),
                    "log": path.name,
                })
                continue
        if section == "hash":
            m = re_hash.match(line)
            if m:
                rows.append({
                    "section": "hash",
                    "policy": m.group("policy").strip(),
                    "time_ms": float(m.group(2)),
                    "cross_dom": int(m.group(3)),
                    "coh_reqs": int(m.group(4)),
                    "evictions": None,
                    "throughput": None,
                    "log": path.name,
                })
                continue


def parse_rq2(path: pathlib.Path, rows):
    """Parses rq2_dir_sizing's two tabular experiments:
      Exp1 BFS:  N  hit_rate  evictions  bfs_time_ns  norm_tput  dir_entries
      Exp2 Btree: keys  hit_rate  evictions  lookup_time_ns  norm_tput  dir_entries
    Both emit a "---" rule row, then floating-point columns. We pick up any row
    that starts with an integer followed by 5 numeric tokens.
    """
    txt = path.read_text(errors="replace")
    lines = txt.splitlines()
    section = None
    RE_ROW = re.compile(
        r"^\s*(\d+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+(\d+)\s*$")
    for line in lines:
        if "Experiment 1: Graph BFS" in line:
            section = "bfs"; continue
        if "Experiment 2: B+ tree" in line or "B-tree" in line:
            section = "btree"; continue
        if "Experiment 3:" in line or "Experiment 4:" in line:
            section = None; continue
        m = RE_ROW.match(line)
        if m and section:
            rows.append({
                "section": section,
                "N": int(m.group(1)),
                "hit_rate": float(m.group(2)),
                "evictions": int(m.group(3)),
                "time_ns": int(m.group(4)),
                "norm_tput": float(m.group(5)),
                "dir_entries": int(m.group(6)),
                "log": path.name,
            })


def write_csv(path, rows, columns):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"  [csv] {path}  rows={len(rows)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logdir", default=str(ARTIFACT / "runs"))
    ap.add_argument("--outdir", default=str(ARTIFACT / "csv"))
    args = ap.parse_args()

    logdir = pathlib.Path(args.logdir)
    outdir = pathlib.Path(args.outdir)

    rows_bfs, rows_btree, rows_hash = [], [], []
    rows_rq2, rows_rq3, rows_rq4alloc, rows_rq4df = [], [], [], []

    # Walk every .stdout.log under the primary logdir AND its sibling sweep
    # directories (rq2_dir_capacity, multigpu_scaling). This keeps the data
    # pipeline resilient when different sub-sweeps write into their own trees.
    search_roots = [logdir]
    parent = logdir.parent
    for sibling in ("rq2_dir_capacity", "multigpu_scaling"):
        sp = parent / sibling
        if sp.exists():
            search_roots.append(sp)

    seen = set()
    all_logs = []
    for root in search_roots:
        all_logs.extend(root.rglob("*.stdout.log"))
    for stdout in sorted(all_logs):
        key = str(stdout.resolve())
        if key in seen: continue
        seen.add(key)
        name = stdout.name
        if name.startswith("rq1_") or name.startswith("rq1."):
            parse_rq1(stdout, rows_bfs, rows_btree, rows_hash)
        elif name.startswith("rq2"):
            parse_rq2(stdout, rows_rq2)
        elif name.startswith("rq3"):
            parse_rq3(stdout, rows_rq3)
        elif name.startswith("rq4_alloc"):
            parse_rq4_alloc(stdout, rows_rq4alloc)
        elif name.startswith("rq4_devfrac"):
            parse_rq4_devfrac(stdout, rows_rq4df)
        else:
            print(f"  [skip] unrecognized log name {name}", file=sys.stderr)

    COH_COLS = ["snoop_hits","snoop_misses","coh_reqs","back_inv",
                "writebacks","evictions","bias_flips",
                "dev_bias_hits","host_bias_hits","dir_entries"]
    write_csv(outdir / "rq1_bfs.csv", rows_bfs,
              ["graph","N","device_fraction","method","method_name",
               "median_us","p25_us","p75_us", *COH_COLS, "log"])
    write_csv(outdir / "rq1_btree.csv", rows_btree,
              ["B","num_leaves","method","median_us","p25_us","p75_us","log"])
    write_csv(outdir / "rq1_hash.csv", rows_hash,
              ["op","median_us","p25_us","p75_us","log"])
    write_csv(outdir / "rq2_dir.csv", rows_rq2,
              ["section","N","hit_rate","evictions","time_ns","norm_tput",
               "dir_entries","log"])
    write_csv(outdir / "rq3_bias.csv", rows_rq3,
              ["source","accel_count","experiment","theta","label",
               "p25","median","p75","unit",
               "snoop_hits","snoop_misses","bias_flips","writebacks",
               "dev_bias_hits","host_bias_hits","back_inv","dir_entries","log"])
    write_csv(outdir / "rq4_alloc.csv", rows_rq4alloc,
              ["section","policy","time_ms","cross_dom","coh_reqs","evictions",
               "throughput","log"])
    write_csv(outdir / "rq4_devfrac.csv", rows_rq4df,
              ["device_fraction","median_ops_sec","p25_ops_sec","p75_ops_sec","log"])


if __name__ == "__main__":
    main()
