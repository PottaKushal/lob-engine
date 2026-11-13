#!/usr/bin/env python3
"""Run the market-making study and write results.

    python python/run_backtest.py --cli build/release/tools/lob_cli [--seeds 30] [--out results] [--img docs/img]

Runs the Avellaneda-Stoikov strategy and a symmetric baseline (same spread, no
inventory skew) over the same seeds, on synthetic flow, through the C++ engine.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobpy.backtest import run_backtest  # noqa: E402
from lobpy.flow import FlowConfig  # noqa: E402
from lobpy.strategy import AvellanedaStoikov, symmetric_baseline  # noqa: E402

BLUE, ORANGE = "#2a78d6", "#eb6834"


def summarise(rows: list[dict]) -> dict:
    out = {}
    for key in rows[0]:
        v = np.array([r[key] for r in rows], dtype=float)
        out[key] = {"mean": float(v.mean()), "std": float(v.std(ddof=1)) if len(v) > 1 else 0.0}
    pnl = np.array([r["final_pnl"] for r in rows])
    out["pnl_mean_over_std_across_seeds"] = float(pnl.mean() / pnl.std(ddof=1)) if len(pnl) > 1 and pnl.std() > 0 else 0.0
    out["pnl_positive_fraction"] = float((pnl > 0).mean())
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/release/tools/lob_cli")
    ap.add_argument("--seeds", type=int, default=30)
    ap.add_argument("--horizon", type=float, default=1800.0)
    ap.add_argument("--out", default="results")
    ap.add_argument("--img", default="")
    a = ap.parse_args()

    cfg = FlowConfig(horizon_s=a.horizon)
    strategies = [AvellanedaStoikov(), symmetric_baseline()]
    t0 = time.time()
    runs = {s.name: [run_backtest(a.cli, s, seed, cfg) for seed in range(1, a.seeds + 1)] for s in strategies}
    metrics = {name: [r.metrics(cfg.dt) for r in rs] for name, rs in runs.items()}
    summary = {name: summarise(m) for name, m in metrics.items()}
    events = sum(r.events for rs in runs.values() for r in rs)

    print(f"{a.seeds} seeds x {cfg.horizon_s:.0f} s simulated, {events:,} engine output events, {time.time() - t0:.1f} s wall")
    print(f"{'metric':32s}" + "".join(f"{n:>26s}" for n in summary))
    for key in ["final_pnl", "inventory_std", "inventory_abs_max", "fill_rate", "lots_traded", "captured_per_lot",
                "markout_per_lot", "adverse_selection_per_lot", "run_sharpe"]:
        print(f"{key:32s}" + "".join(f"{summary[n][key]['mean']:>14.3f} ± {summary[n][key]['std']:<9.3f}" for n in summary))
    for key in ["pnl_mean_over_std_across_seeds", "pnl_positive_fraction"]:
        print(f"{key:32s}" + "".join(f"{summary[n][key]:>26.3f}" for n in summary))

    # Example path (seed 1) downsampled for the web demo.
    step = max(1, len(runs[strategies[0].name][0].t) // 600)
    example = {"t": runs[strategies[0].name][0].t[::step].round(2).tolist(),
               "fair": runs[strategies[0].name][0].fair[::step].round(2).tolist(),
               "series": [{"name": n, "pnl": rs[0].pnl[::step].round(1).tolist(), "inventory": rs[0].inventory[::step].tolist()}
                          for n, rs in runs.items()]}
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "strategy.json"), "w") as f:
        json.dump({"synthetic": True, "seeds": a.seeds, "config": cfg.__dict__, "units": "PnL in ticks x lots",
                   "summary": summary, "final_pnl": {n: [m["final_pnl"] for m in ms] for n, ms in metrics.items()},
                   "example": example}, f, separators=(",", ":"))
    print("wrote", os.path.join(a.out, "strategy.json"))

    if a.img:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False,
                             "axes.grid": True, "grid.alpha": 0.25, "grid.linewidth": 0.6, "figure.dpi": 150})
        os.makedirs(a.img, exist_ok=True)
        colors = dict(zip(runs, [BLUE, ORANGE]))

        fig, ax = plt.subplots(3, 1, figsize=(8, 7.2), sharex=True)
        r0 = runs[strategies[0].name][0]
        ax[0].plot(r0.t / 60, r0.fair, color="#52514e", lw=1.2)
        ax[0].set_ylabel("ticks")
        ax[0].set_title("Synthetic fair value (seed 1)", loc="left", fontsize=10)
        for n, rs in runs.items():
            ax[1].plot(rs[0].t / 60, rs[0].pnl, color=colors[n], lw=1.6, label=n)
            ax[2].plot(rs[0].t / 60, rs[0].inventory, color=colors[n], lw=1.2, label=n)
        ax[1].set_ylabel("tick x lots")
        ax[1].set_title("Mark-to-mid PnL", loc="left", fontsize=10)
        ax[1].legend(frameon=False, loc="upper left")
        ax[2].set_ylabel("lots")
        ax[2].set_title("Inventory", loc="left", fontsize=10)
        ax[2].set_xlabel("minutes")
        ax[2].axhline(0, color="#52514e", lw=0.6)
        fig.suptitle("One run, same synthetic order flow for both strategies", x=0.01, ha="left", fontsize=11, fontweight="bold")
        fig.tight_layout()
        fig.savefig(os.path.join(a.img, "backtest_example.png"))

        fig, ax = plt.subplots(1, 2, figsize=(8, 3.4))
        allp = np.concatenate([[m["final_pnl"] for m in ms] for ms in metrics.values()])
        bins = np.linspace(allp.min(), allp.max(), 16)
        for n, ms in metrics.items():
            ax[0].hist([m["final_pnl"] for m in ms], bins=bins, color=colors[n], alpha=0.75, label=n, edgecolor="white", linewidth=1)
        ax[0].set_title(f"Final PnL across {a.seeds} seeds", loc="left", fontsize=10)
        ax[0].set_xlabel("tick x lots")
        ax[0].set_ylabel("runs")
        ax[0].legend(frameon=False, fontsize=8)
        names = list(metrics)
        vals = [summary[n]["inventory_std"]["mean"] for n in names]
        bars = ax[1].bar(range(len(names)), vals, color=[colors[n] for n in names], width=0.5)
        ax[1].bar_label(bars, fmt="%.1f", padding=2)
        ax[1].set_xticks(range(len(names)), [n.replace(" (", "\n(") for n in names], fontsize=8)
        ax[1].set_title("Inventory standard deviation (lots, mean over seeds)", loc="left", fontsize=10)
        ax[1].grid(axis="x", visible=False)
        fig.tight_layout()
        fig.savefig(os.path.join(a.img, "backtest_summary.png"))
        print("wrote plots to", a.img)


if __name__ == "__main__":
    main()
