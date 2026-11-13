"""Closed-loop backtest: strategy quotes and background flow go through the real engine.

Each step: (1) the strategy re-quotes using only information available at the
start of the step, (2) the background flow for the step is sent, (3) the
engine is synced and its output events update positions and the tracked book.
The strategy's orders rest in the same book as everyone else's, so fills
respect true price-time priority.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np

from .engine import (ACCEPTED, BUY, CANCELLED, POST_ONLY, SELL, TRADE, Engine)
from .flow import Flow, FlowConfig

STRAT_BASE = 1 << 40  # strategy order ids start here; background ids are small
STRAT_OWNER = 1


@dataclass
class Result:
    name: str
    seed: int
    t: np.ndarray           # seconds since the strategy started
    pnl: np.ndarray         # mark-to-mid PnL, tick*lots
    inventory: np.ndarray   # signed lots
    mid: np.ndarray
    fair: np.ndarray
    fills: list = field(default_factory=list)  # (step, side, price, qty, ref_mid)
    quotes_placed: int = 0
    quotes_filled: int = 0
    lots_traded: int = 0
    events: int = 0

    def metrics(self, dt: float, markout_s: float = 5.0) -> dict:
        h = int(round(markout_s / dt))
        cap, mark = [], []
        for step, side, price, qty, ref in self.fills:
            sign = 1 if side == BUY else -1          # we bought (+1) or sold (-1)
            cap.append((sign * (ref - price), qty))  # spread captured versus mid at the time
            j = min(step + h, len(self.mid) - 1)
            mark.append((sign * (self.mid[j] - price), qty))  # profit versus mid markout_s later
        w = sum(q for _, q in cap) or 1
        captured = sum(v * q for v, q in cap) / w
        markout = sum(v * q for v, q in mark) / w
        per_min = np.diff(self.pnl[:: max(1, int(round(60 / dt)))])
        sharpe = float(per_min.mean() / per_min.std(ddof=1) * math.sqrt(len(per_min))) if len(per_min) > 2 and per_min.std() > 0 else 0.0
        return {
            "final_pnl": float(self.pnl[-1]),
            "inventory_std": float(self.inventory.std()),
            "inventory_abs_max": int(np.abs(self.inventory).max()),
            "fills": len(self.fills),
            "lots_traded": self.lots_traded,
            "fill_rate": self.quotes_filled / max(1, self.quotes_placed),
            "captured_per_lot": captured,          # ticks per lot, at fill time
            "markout_per_lot": markout,            # ticks per lot, markout_s later
            "adverse_selection_per_lot": captured - markout,
            "run_sharpe": sharpe,                  # t-statistic of 1-minute PnL increments over the run
        }


def run_backtest(cli: str, strategy, seed: int, cfg: FlowConfig | None = None, quote_qty: int = 2) -> Result:
    cfg = cfg or FlowConfig()
    flow = Flow(cfg, seed)
    n, warm = cfg.steps, int(round(cfg.warmup_s / cfg.dt))
    depth = ({}, {})  # background resting quantity by price, per side
    cash, inv = 0.0, 0
    ref = cfg.f0
    sigma2 = cfg.sigma ** 2  # prior; replaced by an EWMA of realised variance
    quotes = {BUY: None, SELL: None}  # side -> [id, price, remaining, was_filled]
    next_sid = STRAT_BASE
    res = Result(strategy.name, seed, np.zeros(n - warm), np.zeros(n - warm), np.zeros(n - warm, dtype=np.int64),
                 np.zeros(n - warm), flow.fair[warm:n].copy())

    def bg_remove(oid: int, qty: int | None = None) -> None:
        o = flow.live.get(oid)
        if o is None:
            return
        take = o[2] if qty is None else qty
        o[2] -= take
        d = depth[o[0]]
        d[o[1]] -= take
        if d[o[1]] <= 0:
            del d[o[1]]
        if o[2] <= 0:
            del flow.live[oid]

    with Engine(cli, cfg.max_price) as eng:
        for k in range(n):
            ts = int(k * cfg.dt * 1e9)
            # 1. Strategy: decide with information as of the end of the previous step.
            if k >= warm:
                q = strategy.quote(ref, inv, sigma2)
                for side, want in ((BUY, q.bid), (SELL, q.ask)):
                    cur = quotes[side]
                    if cur is not None and (want is None or cur[1] != want or cur[2] <= 0):
                        if cur[2] > 0:
                            eng.cancel(ts, cur[0])
                        quotes[side] = cur = None
                    if cur is None and want is not None:
                        eng.new(ts, next_sid, side, want, quote_qty, POST_ONLY, STRAT_OWNER)
                        quotes[side] = [next_sid, want, quote_qty, False]
                        next_sid += 1
                        res.quotes_placed += 1
            # 2. Background flow for this step.
            for e in flow.step(k):
                if e[0] == "N":
                    eng.new(ts, e[1], e[2], e[3], e[4], e[5])
                else:
                    eng.cancel(ts, e[1])
            # 3. Sync and apply the engine's output.
            out = eng.sync()
            res.events += len(out)
            for oid, other, price, qty, kind, reason, side, _, _ in out[:-1].tolist():
                if kind == TRADE:
                    if other >= STRAT_BASE:  # we were the maker
                        my_side = SELL if side == BUY else BUY
                        cash += price * qty if my_side == SELL else -price * qty
                        inv += -qty if my_side == SELL else qty
                        cur = quotes[my_side]
                        if cur is not None and cur[0] == other:
                            cur[2] -= qty
                            if not cur[3]:
                                cur[3] = True
                                res.quotes_filled += 1
                        res.lots_traded += qty
                        res.fills.append((k - warm, my_side, price, qty, ref))
                    else:
                        bg_remove(other, qty)
                elif kind == ACCEPTED:
                    if oid < STRAT_BASE:
                        flow.live[oid] = [side, price, qty]
                        flow.register(oid)
                        depth[side][price] = depth[side].get(price, 0) + qty
                elif kind == CANCELLED:
                    if oid < STRAT_BASE:
                        bg_remove(oid)
                    else:  # post-only rejected, or our cancel confirmed
                        for s in (BUY, SELL):
                            if quotes[s] is not None and quotes[s][0] == oid:
                                quotes[s][2] = 0
            # 4. Reference price: mid of the book excluding our own quotes.
            if depth[BUY] and depth[SELL]:
                new_ref = 0.5 * (max(depth[BUY]) + min(depth[SELL]))
                sigma2 = 0.98 * sigma2 + 0.02 * (new_ref - ref) ** 2 / cfg.dt
                ref = new_ref
            if k >= warm:
                i = k - warm
                res.t[i] = i * cfg.dt
                res.pnl[i] = cash + inv * ref
                res.inventory[i] = inv
                res.mid[i] = ref
    return res
