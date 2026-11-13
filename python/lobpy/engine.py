"""Thin client for the C++ matching engine.

The engine runs as a child process (`lob_cli stream`). Orders are written to
its stdin in the binary wire format defined in include/lob/protocol.hpp, and
every `sync()` returns the output events produced since the previous sync,
followed by one top-of-book record. Nothing is re-implemented in Python: all
matching, queue priority and self-trade logic is the C++ book.
"""
from __future__ import annotations

import os
import struct
import subprocess

import numpy as np

BUY, SELL = 0, 1
LIMIT, MARKET, IOC, FOK, POST_ONLY = 0, 1, 2, 3, 4
ACCEPTED, TRADE, CANCELLED, REJECTED, MODIFIED, BOOK_TOP = 0, 1, 2, 3, 4, 5

_HEADER = struct.Struct("<4sHHII")
_ADD = struct.Struct("<cQQIiIcB")
_CANCEL = struct.Struct("<cQQ")
_MODIFY = struct.Struct("<cQQiI")
assert (_HEADER.size, _ADD.size, _CANCEL.size, _MODIFY.size) == (16, 31, 17, 25)

OUT_DTYPE = np.dtype(
    [("id", "<u8"), ("other", "<u8"), ("price", "<i4"), ("qty", "<u4"),
     ("kind", "u1"), ("reason", "u1"), ("side", "u1"), ("pad", "u1"), ("pad2", "<u4")]
)
assert OUT_DTYPE.itemsize == 32


class Engine:
    def __init__(self, cli_path: str, max_price: int = 20000, max_orders: int = 1 << 16):
        self._p = subprocess.Popen([cli_path, "stream"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self._buf = bytearray(_HEADER.pack(b"LOB1", 1, 0, max_price, max_orders))
        self._fd = self._p.stdout.fileno()

    def new(self, ts: int, oid: int, side: int, price: int, qty: int, otype: int = LIMIT, owner: int = 0) -> None:
        self._buf += _ADD.pack(b"A", ts, oid, owner, price, qty, b"B" if side == BUY else b"S", otype)

    def cancel(self, ts: int, oid: int) -> None:
        self._buf += _CANCEL.pack(b"X", ts, oid)

    def modify(self, ts: int, oid: int, price: int, qty: int) -> None:
        self._buf += _MODIFY.pack(b"M", ts, oid, price, qty)

    def sync(self) -> np.ndarray:
        """Flush pending messages; return output events. The last record is the top of book:
        price = best bid, qty = best ask (0 if none), id / other = quantity at each."""
        self._buf += b"S"
        self._p.stdin.write(self._buf)
        self._p.stdin.flush()
        self._buf = bytearray()
        data = bytearray()
        while True:
            chunk = os.read(self._fd, 1 << 16)
            if not chunk:
                raise RuntimeError("engine exited unexpectedly")
            data += chunk
            if len(data) % 32 == 0 and data[-8] == BOOK_TOP:  # kind byte of the final record
                break
        return np.frombuffer(bytes(data), dtype=OUT_DTYPE)

    def close(self) -> None:
        if self._p.poll() is None:
            self._p.stdin.close()
            self._p.wait(timeout=10)
        self._p.stdout.close()

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()
