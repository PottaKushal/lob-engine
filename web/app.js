// Static visualizer. All data is JSON produced by the C++ engine at build time.
"use strict";
const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};
const num = (v, d = 0) => Number(v).toLocaleString("en-US", { minimumFractionDigits: d, maximumFractionDigits: d });
const px = (ticks) => (ticks / 100).toFixed(2);
const getJSON = (url) => fetch(url).then((r) => { if (!r.ok) throw new Error(url); return r.json(); });
const cssVar = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

// ---- theme -------------------------------------------------------------
const redraws = [];
$("theme").addEventListener("click", () => {
  const root = document.documentElement;
  const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
  root.dataset.theme = dark ? "light" : "dark";
  redraws.forEach((f) => f());
});
let resizeTimer;
addEventListener("resize", () => { clearTimeout(resizeTimer); resizeTimer = setTimeout(() => redraws.forEach((f) => f()), 120); });

// ---- tooltip -----------------------------------------------------------
const tip = $("tip");
function showTip(text, x, y) {
  tip.textContent = text;
  tip.hidden = false;
  const w = tip.offsetWidth, h = tip.offsetHeight;
  tip.style.left = Math.max(4, Math.min(innerWidth - w - 4, x + 12)) + "px";
  tip.style.top = Math.max(4, y - h - 10) + "px";
}
const hideTip = () => { tip.hidden = true; };

// ---- line chart (SVG, one y axis, crosshair tooltip) ---------------------
function lineChart(host, cfg) {
  const NS = "http://www.w3.org/2000/svg";
  const mk = (tag, attrs) => { const n = document.createElementNS(NS, tag); for (const k in attrs) n.setAttribute(k, attrs[k]); return n; };
  function draw() {
    const W = host.clientWidth, H = host.clientHeight;
    if (!W || !H) return;
    const m = { l: 46, r: 10, t: 8, b: 20 };
    const xs = cfg.x, n = xs.length;
    let lo = Infinity, hi = -Infinity;
    for (const s of cfg.series) for (const v of s.values) { if (v < lo) lo = v; if (v > hi) hi = v; }
    if (cfg.zero) { lo = Math.min(lo, 0); hi = Math.max(hi, 0); }
    if (hi === lo) { hi += 1; lo -= 1; }
    const pad = (hi - lo) * 0.06; lo -= pad; hi += pad;
    const X = (i) => m.l + (n > 1 ? (i / (n - 1)) * (W - m.l - m.r) : 0);
    const Y = (v) => m.t + (1 - (v - lo) / (hi - lo)) * (H - m.t - m.b);
    const svg = mk("svg", { viewBox: `0 0 ${W} ${H}`, role: "img", "aria-label": cfg.label || "chart" });
    for (let k = 0; k <= 3; k++) {
      const v = lo + ((hi - lo) * k) / 3, y = Y(v);
      svg.appendChild(mk("line", { x1: m.l, x2: W - m.r, y1: y, y2: y, stroke: cssVar("--grid"), "stroke-width": 1 }));
      const t = mk("text", { x: m.l - 6, y: y + 4, "text-anchor": "end" });
      t.textContent = cfg.fmtY ? cfg.fmtY(v) : num(v);
      svg.appendChild(t);
    }
    if (cfg.zero) svg.appendChild(mk("line", { x1: m.l, x2: W - m.r, y1: Y(0), y2: Y(0), stroke: cssVar("--text3"), "stroke-width": 1 }));
    [[0, "start"], [n - 1, "end"]].forEach(([i, anchor]) => {
      const t = mk("text", { x: X(i), y: H - 4, "text-anchor": anchor });
      t.textContent = cfg.fmtX ? cfg.fmtX(xs[i]) : xs[i];
      svg.appendChild(t);
    });
    for (const s of cfg.series) {
      let d = "";
      for (let i = 0; i < n; i++) d += (i ? "L" : "M") + X(i).toFixed(1) + " " + Y(s.values[i]).toFixed(1);
      svg.appendChild(mk("path", { d, fill: "none", stroke: cssVar(s.color), "stroke-width": 2, "stroke-linejoin": "round" }));
    }
    const cross = mk("line", { y1: m.t, y2: H - m.b, stroke: cssVar("--text3"), "stroke-width": 1, visibility: "hidden" });
    svg.appendChild(cross);
    const dots = cfg.series.map((s) => svg.appendChild(mk("circle", { r: 4, fill: cssVar(s.color), stroke: cssVar("--surface"), "stroke-width": 2, visibility: "hidden" })));
    const move = (ev) => {
      const r = svg.getBoundingClientRect();
      const cx = (ev.touches ? ev.touches[0].clientX : ev.clientX), cy = (ev.touches ? ev.touches[0].clientY : ev.clientY);
      const i = Math.max(0, Math.min(n - 1, Math.round(((cx - r.left - m.l) / (W - m.l - m.r)) * (n - 1))));
      cross.setAttribute("x1", X(i)); cross.setAttribute("x2", X(i)); cross.setAttribute("visibility", "visible");
      let text = cfg.fmtX ? cfg.fmtX(xs[i]) : String(xs[i]);
      cfg.series.forEach((s, k) => {
        dots[k].setAttribute("cx", X(i)); dots[k].setAttribute("cy", Y(s.values[i])); dots[k].setAttribute("visibility", "visible");
        text += `\n${s.name}: ${cfg.fmtY ? cfg.fmtY(s.values[i]) : num(s.values[i])}`;
      });
      showTip(text, cx, cy);
    };
    const leave = () => { hideTip(); cross.setAttribute("visibility", "hidden"); dots.forEach((d) => d.setAttribute("visibility", "hidden")); };
    svg.addEventListener("mousemove", move);
    svg.addEventListener("touchmove", move, { passive: true });
    svg.addEventListener("mouseleave", leave);
    svg.addEventListener("touchend", leave);
    host.replaceChildren(svg);
  }
  draw();
  return draw;
}

// ---- horizontal bars and tables -----------------------------------------
function bars(host, rows) {
  const max = Math.max(...rows.map((r) => r.value));
  host.replaceChildren(...rows.map((r) => {
    const row = el("div", "brow");
    row.appendChild(el("div", "name", r.name));
    const track = el("div", "track");
    const fill = el("div", "fill");
    fill.style.width = `${(r.value / max) * 72}%`;
    const val = el("div", "val", r.text);
    if (r.sub) val.appendChild(el("small", "", " " + r.sub));
    track.append(fill, val);
    row.title = r.title || "";
    row.appendChild(track);
    return row;
  }));
}
function table(host, head, rows) {
  const thead = el("thead"), tr = el("tr");
  head.forEach((h) => tr.appendChild(el("th", "", h)));
  thead.appendChild(tr);
  const tbody = el("tbody");
  rows.forEach((r) => { const t = el("tr"); r.forEach((c) => t.appendChild(el("td", "", c))); tbody.appendChild(t); });
  host.replaceChildren(thead, tbody);
}

// ---- depth ladder --------------------------------------------------------
function ladder(host, bids, asks, maxQty, mine) {
  const rows = [];
  const row = (cls, p, q) => {
    const r = el("div", "lrow " + cls + (mine && mine.has(p) ? " mine" : ""));
    r.appendChild(el("span", "px", px(p)));
    const bar = el("span", "bar");
    bar.style.width = Math.max(1, Math.sqrt(q / maxQty) * 100) + "%";
    const cell = el("span"); cell.appendChild(bar);
    r.append(cell, el("span", "q", num(q)));
    return r;
  };
  for (let i = asks.length - 1; i >= 0; i--) rows.push(row("ask", asks[i][0], asks[i][1]));
  const s = el("div", "spread");
  s.textContent = bids.length && asks.length
    ? `spread ${px(asks[0][0] - bids[0][0])}   mid ${((asks[0][0] + bids[0][0]) / 200).toFixed(3)}`
    : "one side of the book is empty";
  rows.push(s);
  for (const b of bids) rows.push(row("bid", b[0], b[1]));
  host.replaceChildren(...rows);
}

// ---- benchmarks ----------------------------------------------------------
const WL_NOTES = {
  books_dense: (w) => `Dense book: ${num(w.dense.events)} synthetic events (${num(w.dense.trades)} trades), about ${num(w.dense.resting_orders_end)} resting orders on ${num(w.dense.levels_end)} price levels.`,
  books_sparse: (w) => `Sparse book: ${num(w.sparse.events)} synthetic events, about ${num(w.sparse.resting_orders_end)} resting orders spread over ${num(w.sparse.levels_end)} price levels, so most levels are empty.`,
  books_gap: () => "Worst case for a flat price array: the best bid is cancelled again and again while the next bid is 59,900 ticks away. A linear scan pays for the whole gap; the two-level bitmap does not.",
};
let bench = null, workload = "books_dense";
function renderBench() {
  if (!bench) return;
  const e = bench.env, d = bench.books_dense;
  const v1 = d[0], best = d[d.length - 1];
  const tiles = [
    [num(best.mps.median, 1), "M/s", `events matched per second, single thread (${best.name})`],
    [num(best.mps.median / v1.mps.median, 1), "x", `faster than the std::map + std::list version on the same stream`],
    [num(best.latency_ns.p99), "ns", `p99 per-event latency (${best.name}); max ${num(best.latency_ns.max)} ns`],
    ["8M", "events", "checked against a brute-force reference book on every test run"],
  ];
  $("tiles").replaceChildren(...tiles.map(([v, u, l]) => {
    const t = el("div", "tile"), vv = el("div", "v", v);
    vv.appendChild(el("small", "", u));
    t.append(vv, el("div", "l", l));
    return t;
  }));
  $("env").textContent = `${e.cpu}, ${e.cores} logical cores, ${e.os}. ${e.compiler}, ${e.build} build, flags ${e.flags.trim()}. `
    + `${e.runs} timed runs after one warmup, seed ${e.seed}, ${e.date}. Clock resolution ${e.clock_resolution_ns} ns.`;
  const rows = bench[workload];
  $("wl-note").textContent = WL_NOTES[workload](bench.workloads);
  bars($("bars"), rows.map((r) => ({
    name: r.name, value: r.mps.median, text: num(r.mps.median, 2),
    sub: `(${num(r.mps.min, 2)} to ${num(r.mps.max, 2)})`, title: r.desc,
  })));
  table($("lat-table"), ["Version", "mean", "p50", "p90", "p99", "p99.9", "p99.99", "max"], rows.map((r) => {
    const l = r.latency_ns;
    return [r.name, num(l.mean, 1), num(l.p50), num(l.p90), num(l.p99), num(l.p999), num(l.p9999), num(l.max)];
  }));
  $("lat-note").textContent = `Each event is timed with two clock reads (about ${num(e.clock_cost_ns)} ns each) at ${e.clock_resolution_ns} ns resolution, `
    + `so values near the resolution are quantised${e.clock_resolution_ns > 10 ? " and a p50 of 0 means \"below one clock tick\"" : ""}. `
    + "Throughput above is measured separately, without per-event clock calls. Tail values on a desktop OS include scheduler noise.";
  bars($("wirebars"), [
    { name: "1 thread", value: bench.wire.serial_mps.median, text: num(bench.wire.serial_mps.median, 2), sub: "(median)" },
    { name: "3 threads, 2 queues", value: bench.wire.pipeline_mps.median, text: num(bench.wire.pipeline_mps.median, 2), sub: "(median)" },
  ]);
  table($("paced-table"), ["Msgs/s", "p50", "p99", "p99.9", "p99.99", "max"], bench.pipeline_latency.map((p) => {
    const l = p.latency_ns;
    return [num(p.rate), num(l.p50), num(l.p99), num(l.p999), num(l.p9999), num(l.max)];
  }));
}
document.querySelectorAll("#wl-tabs button").forEach((b) => b.addEventListener("click", () => {
  document.querySelectorAll("#wl-tabs button").forEach((x) => x.setAttribute("aria-selected", String(x === b)));
  workload = b.dataset.wl;
  renderBench();
}));
getJSON("data/manifest.json").then(async (m) => {
  const sets = await Promise.all(m.bench.map((f) => getJSON("data/" + f).then((j) => ({ f, j }))));
  const sel = $("dataset");
  sets.forEach(({ f, j }, i) => {
    const o = el("option", "", `${j.env.cpu} (${j.env.os.split(" ")[0]}${f.includes("live") ? ", this site's build machine" : ""})`);
    o.value = String(i);
    sel.appendChild(o);
  });
  const pick = () => { bench = sets[Number(sel.value)].j; renderBench(); };
  sel.addEventListener("change", pick);
  const preferred = sets.findIndex((s) => !s.f.includes("live"));
  sel.value = String(preferred >= 0 ? preferred : 0);
  pick();
}).catch(() => { $("env").textContent = "Benchmark data is not available in this build."; });

// ---- replay ---------------------------------------------------------------
getJSON("data/replay.json").then((rp) => {
  const frames = rp.frames, N = frames.length;
  let maxQty = 1, trades = 0;
  const mids = frames.map((f) => (f.b.length && f.a.length ? (f.b[0][0] + f.a[0][0]) / 200 : NaN));
  for (let i = 0; i < N; i++) { if (isNaN(mids[i])) mids[i] = i ? mids[i - 1] : 0; }
  for (const f of frames) for (const l of f.b.concat(f.a)) if (l[1] > maxQty) maxQty = l[1];
  const cum = frames.map((f) => (trades += f.n));
  const scrub = $("scrub");
  scrub.max = String(N - 1);
  let i = 0, playing = true, last = 0, tape = [];
  const WIN = 160;
  let drawMid = () => {};
  function render() {
    const f = frames[i];
    ladder($("ladder"), f.b, f.a, maxQty);
    for (const t of f.t) tape.unshift(t);
    tape = tape.slice(0, 8);
    $("tape").replaceChildren(...tape.map((t) => {
      const d = el("div", t[2] === 0 ? "buy" : "sell");
      d.append(el("span", "side", t[2] === 0 ? "BUY" : "SELL"), el("span", "", px(t[0])), el("span", "dim", "x " + num(t[1])));
      return d;
    }));
    const a = Math.max(0, i - WIN + 1);
    const xs = [], vs = [];
    for (let k = a; k <= i; k++) { xs.push((k + 21) * rp.every); vs.push(mids[k]); }
    drawMid = lineChart($("midchart"), { x: xs, series: [{ name: "mid", color: "--s1", values: vs }], fmtY: (v) => v.toFixed(2), fmtX: (v) => "event " + num(v), label: "Mid price over recent events" });
    $("replaystat").textContent = `Event ${num((i + 21) * rp.every)} of ${num((N + 20) * rp.every)}. ${num(cum[i])} trades so far.`;
    scrub.value = String(i);
  }
  redraws.push(() => drawMid());
  function tick(ts) {
    if (playing && ts - last > 420 / Number($("speed").value)) {
      last = ts;
      i = (i + 1) % N;
      if (i === 0) tape = [];
      render();
    }
    requestAnimationFrame(tick);
  }
  $("play").addEventListener("click", () => { playing = !playing; $("play").textContent = playing ? "Pause" : "Play"; });
  scrub.addEventListener("input", () => { i = Number(scrub.value); tape = []; render(); });
  if (matchMedia("(prefers-reduced-motion: reduce)").matches) { playing = false; $("play").textContent = "Play"; }
  render();
  requestAnimationFrame(tick);
}).catch(() => { $("replaystat").textContent = "Replay data is not available in this build."; });

// ---- strategy ---------------------------------------------------------------
getJSON("data/strategy.json").then((st) => {
  const ex = st.example, colors = ["--s1", "--s2"];
  const fmtT = (t) => (t / 60).toFixed(1) + " min";
  $("strat-legend").replaceChildren(...ex.series.flatMap((s, k) => [el("i", "sw s" + (k + 1)), document.createTextNode(s.name)]));
  redraws.push(lineChart($("pnlchart"), { x: ex.t, zero: true, fmtX: fmtT, label: "PnL over time",
    series: ex.series.map((s, k) => ({ name: s.name, color: colors[k], values: s.pnl })) }));
  redraws.push(lineChart($("invchart"), { x: ex.t, zero: true, fmtX: fmtT, label: "Inventory over time",
    series: ex.series.map((s, k) => ({ name: s.name, color: colors[k], values: s.inventory })) }));
  const names = Object.keys(st.summary);
  const ms = (k, d = 1) => names.map((n) => `${num(st.summary[n][k].mean, d)} ± ${num(st.summary[n][k].std, d)}`);
  $("strat-title").textContent = `Across ${st.seeds} seeds of ${num(st.config.horizon_s / 60)} simulated minutes each (mean ± standard deviation)`;
  table($("strat-table"), ["Metric"].concat(names), [
    ["Final PnL, tick x lots"].concat(ms("final_pnl", 0)),
    ["Runs with positive PnL"].concat(names.map((n) => num(st.summary[n].pnl_positive_fraction * 100) + "%")),
    ["Mean PnL / std of PnL across seeds"].concat(names.map((n) => num(st.summary[n].pnl_mean_over_std_across_seeds, 2))),
    ["Inventory std, lots"].concat(ms("inventory_std", 2)),
    ["Largest absolute inventory, lots"].concat(ms("inventory_abs_max", 1)),
    ["Quotes that received a fill"].concat(names.map((n) => num(st.summary[n].fill_rate.mean * 100, 1) + "%")),
    ["Spread captured at fill, ticks per lot"].concat(ms("captured_per_lot", 2)),
    ["Value 5 s after fill, ticks per lot"].concat(ms("markout_per_lot", 2)),
    ["Adverse selection, ticks per lot"].concat(ms("adverse_selection_per_lot", 2)),
  ]);
}).catch(() => { $("strat-title").textContent = "Strategy data is not available in this build."; });

// ---- the real engine in WebAssembly -------------------------------------------
(function loadWasm() {
  const s = document.createElement("script");
  s.src = "lob_wasm.js";
  s.onerror = () => {};  // panel stays hidden when the wasm build is absent
  s.onload = async () => {
    let M;
    try { M = await createLob(); } catch (e) { return; }
    $("try").hidden = false;
    const KIND = ["ACCEPTED", "TRADE", "CANCELLED", "REJECTED", "MODIFIED"];
    const REASON = ["", "cancelled by user", "unfilled remainder", "FOK not fillable", "post-only would cross", "self-trade prevention",
      "replaced", "duplicate id", "bad price", "bad quantity", "unknown order", "book full"];
    const mine = new Map();  // id -> {side, price}
    const log = [];
    const say = (text, cls) => { log.unshift([text, cls]); log.length = Math.min(log.length, 40); };
    function readOut() {
      const n = M._lob_out_count(), base = M._lob_out_ptr();
      const dv = new DataView(M.HEAPU8.buffer, base, n * 32);
      const out = [];
      for (let k = 0; k < n; k++) {
        const o = k * 32;
        out.push({ id: Number(dv.getBigUint64(o, true)), other: Number(dv.getBigUint64(o + 8, true)), price: dv.getInt32(o + 16, true),
          qty: dv.getUint32(o + 20, true), kind: dv.getUint8(o + 24), reason: dv.getUint8(o + 25), side: dv.getUint8(o + 26) });
      }
      return out;
    }
    function draw() {
      const base = M._lob_depth(12) >> 2, H = M.HEAP32, bids = [], asks = [];
      for (let k = 0; k < 12; k++) {
        if (H[base + k * 2]) bids.push([H[base + k * 2], H[base + k * 2 + 1]]);
        if (H[base + (16 + k) * 2]) asks.push([H[base + (16 + k) * 2], H[base + (16 + k) * 2 + 1]]);
      }
      let maxQty = 1;
      for (const l of bids.concat(asks)) maxQty = Math.max(maxQty, l[1]);
      for (const [id] of mine) if (M._lob_open_qty(id) === 0) mine.delete(id);
      ladder($("ladder2"), bids, asks, maxQty, new Set([...mine.values()].map((m) => m.price)));
      $("log").replaceChildren(...(log.length ? log : [["Submit an order to see the engine's output events.", "dim"]]).map(([t, c]) => {
        const d = el("div", c); d.appendChild(el("span", "", t)); return d;
      }));
      $("mine").replaceChildren(...(mine.size ? [...mine.entries()].map(([id, m]) => {
        const d = el("div", m.side ? "sell" : "buy");
        d.append(el("span", "side", m.side ? "SELL" : "BUY"), el("span", "", `${px(m.price)} x ${M._lob_open_qty(id)}`));
        const b = el("button", "ghost", "Cancel");
        b.style.padding = "0 8px";
        b.addEventListener("click", () => { M._lob_cancel(id); report(readOut()); draw(); });
        d.appendChild(b);
        return d;
      }) : [el("div", "dim", "none")]));
    }
    function report(out) {
      for (const o of out) {
        if (o.kind === 1) say(`TRADE ${num(o.qty)} @ ${px(o.price)}`, o.side === 0 ? "buy" : "sell");
        else say(`${KIND[o.kind]} ${num(o.qty)} @ ${o.price ? px(o.price) : "market"}${o.reason ? " (" + REASON[o.reason] + ")" : ""}`, "dim");
      }
    }
    function reset() {
      M._lob_reset(7);
      M._lob_seed(4000);
      mine.clear();
      log.length = 0;
      const mid = Math.round((M._lob_best_bid() + M._lob_best_ask()) / 2);
      $("order").elements.price.value = String(mid);
      draw();
    }
    $("order").addEventListener("submit", (ev) => {
      ev.preventDefault();
      const f = ev.target.elements;
      const side = Number(f.side.value), price = Number(f.price.value);
      const id = M._lob_submit(side, Number(f.type.value), price, Number(f.qty.value), 1);
      const out = readOut();
      report(out);
      if (out.some((o) => o.kind === 0)) mine.set(id, { side, price });
      draw();
    });
    $("flow").addEventListener("click", () => { const t = M._lob_seed(200); say(`background flow: 200 events, ${t} trades`, "dim"); draw(); });
    $("reset").addEventListener("click", reset);
    reset();
  };
  document.head.appendChild(s);
})();
