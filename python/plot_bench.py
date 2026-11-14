#!/usr/bin/env python3
"""Plot throughput by version from a lob_bench JSON file.

    python python/plot_bench.py results/bench-apple-m4.json docs/img/bench_throughput.png
"""
import json
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src, dst = sys.argv[1], sys.argv[2]
d = json.load(open(src))
plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "figure.dpi": 150})
panels = [("books_dense", "Dense book"), ("books_sparse", "Sparse book"), ("books_gap", "Worst-case gap")]
fig, axes = plt.subplots(1, 3, figsize=(11, 3.2))
for ax, (key, title) in zip(axes, panels):
    rows = d[key][::-1]
    vals = [r["mps"]["median"] for r in rows]
    bars = ax.barh([r["name"] for r in rows], vals, color="#2a78d6", height=0.55)
    ax.bar_label(bars, labels=[f"{v:.2f}" for v in vals], padding=3)
    ax.set_xlim(0, max(vals) * 1.22)
    ax.set_title(title, loc="left", fontsize=10)
    ax.set_xlabel("million events / s")
    ax.grid(axis="x", alpha=0.25, linewidth=0.6)
    if ax is not axes[0]:
        ax.set_yticklabels([])
e = d["env"]
fig.suptitle(f"Matching-core throughput by version. {e['cpu']}, {e['compiler']}, {e['flags'].strip()}; median of {e['runs']} runs",
             x=0.01, ha="left", fontsize=10, fontweight="bold")
fig.tight_layout()
fig.savefig(dst)
print("wrote", dst)
