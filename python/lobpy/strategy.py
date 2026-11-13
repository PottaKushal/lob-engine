"""Quoting strategies. See docs/STRATEGY.md for the derivation and the caveats.

Avellaneda and Stoikov (2008) model a dealer with exponential (CARA) utility
and risk aversion gamma, a mid price that is Brownian with variance sigma^2
per second, and market orders that hit a quote at distance delta from the mid
with Poisson intensity A * exp(-k * delta). To first order the optimal quotes
are centred on a *reservation price*

    r = s - q * gamma * sigma^2 * tau

(s = reference mid, q = signed inventory, tau = time to horizon) with total
spread

    spread = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / k).

The first term of the spread and the whole inventory skew come from price
risk; the second term is the spread that maximises expected fill profit
against the exponential fill curve. We use a constant tau (the stationary,
"infinite horizon" simplification) so quotes do not collapse at the end of
the run.
"""
from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass
class Quote:
    bid: int | None
    ask: int | None


@dataclass
class AvellanedaStoikov:
    gamma: float = 0.05      # risk aversion, per (tick * lot)
    k: float = 0.7           # decay of fill intensity with distance, per tick
    tau: float = 10.0        # seconds; constant risk horizon
    max_inventory: int = 40  # hard limit, lots
    skew: bool = True        # False gives the symmetric baseline with the same spread
    name: str = "Avellaneda-Stoikov"

    def half_spread(self, sigma2: float) -> float:
        return 0.5 * self.gamma * sigma2 * self.tau + math.log(1.0 + self.gamma / self.k) / self.gamma

    def quote(self, ref: float, inventory: int, sigma2: float) -> Quote:
        r = ref - (inventory * self.gamma * sigma2 * self.tau if self.skew else 0.0)
        h = self.half_spread(sigma2)
        bid = math.floor(r - h)
        ask = math.ceil(r + h)
        if ask <= bid:
            ask = bid + 1
        return Quote(None if inventory >= self.max_inventory else bid,
                     None if inventory <= -self.max_inventory else ask)


def symmetric_baseline(**kw) -> AvellanedaStoikov:
    """Same spread, no inventory skew: isolates what the reservation price buys."""
    return AvellanedaStoikov(skew=False, name="Symmetric (no skew)", **kw)
