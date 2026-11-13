"""Synthetic background order flow. THIS IS NOT MARKET DATA.

A stylised "zero-intelligence plus informed traders" model, chosen because
every moving part is a textbook stochastic process that can be stated in one
line, and because it produces the two things a market maker has to manage:
inventory risk and adverse selection.

Per time step dt (all processes independent unless stated):

* Fair value F_t follows a driftless Gaussian random walk,
  F_{t+dt} = F_t + sigma * sqrt(dt) * Z,  Z ~ N(0, 1), in ticks.
* Passive limit orders arrive on each side as a Poisson process with rate
  `limit_rate` per second. A buy is priced at round(F) - d, a sell at
  round(F) + d, with d = min_dist + Geometric(depth_p). Because they are
  priced off the current fair value, they cross (and trade with) stale quotes
  after F has moved, which is what drags the book along with F.
* Every resting background order is cancelled independently with hazard
  `cancel_rate` per second, so over a step each survives with probability
  exp(-cancel_rate * dt). In steady state the book holds about
  2 * limit_rate / cancel_rate orders.
* Uninformed market orders arrive on each side as Poisson(`market_rate`).
* Informed market orders arrive as Poisson(`informed_rate`) and always trade
  in the direction of the *future* move F_{t+horizon} - F_t. They are the
  source of adverse selection: after they hit a quote, the price tends to
  move against whoever was quoting.
* Order sizes are uniform on 1..max_qty lots.

Optional clustering (`hawkes_alpha > 0`): the uninformed market-order rate is
self-exciting, lambda_t = market_rate + excitation_t, where every market order
adds `hawkes_alpha * hawkes_beta` to the excitation and the excitation decays
as exp(-hawkes_beta * t). This is a discretised exponential-kernel Hawkes
process with branching ratio hawkes_alpha (must be < 1 for stationarity).
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from .engine import BUY, SELL, LIMIT, MARKET


@dataclass
class FlowConfig:
    dt: float = 0.25              # seconds per step
    horizon_s: float = 1800.0     # simulated seconds
    warmup_s: float = 30.0        # book builds up before the strategy starts
    f0: float = 10000.0           # initial fair value, ticks
    sigma: float = 1.5            # fair-value volatility, ticks per sqrt(second)
    limit_rate: float = 12.0      # passive orders per second, per side
    cancel_rate: float = 0.15     # per-order cancel hazard, per second
    market_rate: float = 1.2      # uninformed market orders per second, per side
    informed_rate: float = 1.0    # informed market orders per second (total)
    informed_horizon_s: float = 5.0
    min_dist: int = 2             # closest a passive order sits to fair value, ticks
    depth_p: float = 0.25         # geometric parameter for extra distance
    max_qty: int = 5
    hawkes_alpha: float = 0.0     # 0 disables clustering
    hawkes_beta: float = 2.0
    max_price: int = 20000

    @property
    def steps(self) -> int:
        return int(round((self.warmup_s + self.horizon_s) / self.dt))


class Flow:
    """Generates one step of background events at a time."""

    def __init__(self, cfg: FlowConfig, seed: int):
        self.cfg = cfg
        self.rng = np.random.default_rng(seed)
        n = cfg.steps
        self.fair = cfg.f0 + np.cumsum(cfg.sigma * np.sqrt(cfg.dt) * self.rng.standard_normal(n + 1))
        self._h = max(1, int(round(cfg.informed_horizon_s / cfg.dt)))
        self._excite = 0.0
        self.next_id = 1
        self.live: dict[int, list] = {}  # id -> [side, price, qty]; maintained by the backtest from engine output
        self._ids: list[int] = []        # candidate ids for cancellation (may contain dead ids)

    def register(self, oid: int) -> None:
        self._ids.append(oid)

    def step(self, k: int) -> list[tuple]:
        """Events for step k as tuples: ('N', id, side, price, qty, type) or ('X', id)."""
        c, rng = self.cfg, self.rng
        f = self.fair[k]
        mid = int(round(f))
        ev: list[tuple] = []
        for side in (BUY, SELL):
            for _ in range(rng.poisson(c.limit_rate * c.dt)):
                d = c.min_dist + int(rng.geometric(c.depth_p)) - 1
                px = mid - d if side == BUY else mid + d
                ev.append(("N", self._new_id(), side, min(max(px, 1), c.max_price), int(rng.integers(1, c.max_qty + 1)), LIMIT))
        # Cancellations: thin the candidate list with the per-step death probability.
        p_cancel = 1.0 - np.exp(-c.cancel_rate * c.dt)
        self._ids = [i for i in self._ids if i in self.live]
        if self._ids:
            for i in np.flatnonzero(rng.random(len(self._ids)) < p_cancel):
                ev.append(("X", self._ids[i]))
        # Uninformed market orders (optionally self-exciting).
        self._excite *= np.exp(-c.hawkes_beta * c.dt)
        n_mkt = 0
        for side in (BUY, SELL):
            n = rng.poisson((c.market_rate + self._excite / 2) * c.dt)
            n_mkt += n
            for _ in range(n):
                ev.append(("N", self._new_id(), side, 0, int(rng.integers(1, c.max_qty + 1)), MARKET))
        self._excite += c.hawkes_alpha * c.hawkes_beta * n_mkt
        # Informed market orders trade toward where fair value is going.
        future = self.fair[min(k + self._h, len(self.fair) - 1)]
        if abs(future - f) >= 1.0:
            side = BUY if future > f else SELL
            for _ in range(rng.poisson(c.informed_rate * c.dt)):
                ev.append(("N", self._new_id(), side, 0, int(rng.integers(1, c.max_qty + 1)), MARKET))
        order = rng.permutation(len(ev))
        return [ev[i] for i in order]

    def _new_id(self) -> int:
        i = self.next_id
        self.next_id += 1
        return i
