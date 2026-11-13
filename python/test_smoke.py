#!/usr/bin/env python3
"""Smoke test for the Python layer (no test framework needed).

    python python/test_smoke.py build/release/tools/lob_cli
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobpy.backtest import run_backtest
from lobpy.engine import BOOK_TOP, BUY, IOC, SELL, TRADE, Engine
from lobpy.flow import FlowConfig
from lobpy.strategy import AvellanedaStoikov, symmetric_baseline

cli = sys.argv[1] if len(sys.argv) > 1 else "build/release/tools/lob_cli"

# 1. Wire protocol round trip: two resting sells, one IOC buy sweeping both in price-time order.
with Engine(cli, max_price=1000, max_orders=64) as e:
    e.new(1, 1, SELL, 101, 5)
    e.new(2, 2, SELL, 100, 5)
    e.new(3, 3, BUY, 101, 7, IOC)
    out = e.sync()
    trades = [(int(r["other"]), int(r["price"]), int(r["qty"])) for r in out if r["kind"] == TRADE]
    assert trades == [(2, 100, 5), (1, 101, 2)], trades
    top = out[-1]
    assert top["kind"] == BOOK_TOP and top["price"] == 0 and top["qty"] == 101 and top["other"] == 3, top

# 2. Backtest is deterministic for a given seed, and position accounting is consistent.
cfg = FlowConfig(horizon_s=120.0)
a = run_backtest(cli, AvellanedaStoikov(), 3, cfg)
b = run_backtest(cli, AvellanedaStoikov(), 3, cfg)
assert (a.pnl == b.pnl).all() and (a.inventory == b.inventory).all()
net = sum(q if side == BUY else -q for _, side, _, q, _ in a.fills)
assert net == a.inventory[-1], (net, a.inventory[-1])
assert a.lots_traded > 0

# 3. The inventory skew should hold less inventory than the symmetric baseline on the same flow.
cfg = FlowConfig(horizon_s=600.0)
skew = [run_backtest(cli, AvellanedaStoikov(), s, cfg).inventory.std() for s in (1, 2, 3)]
flat = [run_backtest(cli, symmetric_baseline(), s, cfg).inventory.std() for s in (1, 2, 3)]
assert sum(skew) < sum(flat), (skew, flat)
print("python smoke test OK")
