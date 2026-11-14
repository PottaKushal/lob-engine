#!/usr/bin/env bash
# Builds the static demo site into _site/. Every data file is produced by the
# engine itself; there is no server. Used by the Pages workflow and locally.
#
#   scripts/build_site.sh [--quick]     quick = smaller benchmark, for a fast local preview
set -euo pipefail
cd "$(dirname "$0")/.."

QUICK=""
[[ "${1:-}" == "--quick" ]] && QUICK="--quick"
PY="${PYTHON:-python3}"
[[ -x .venv/bin/python ]] && PY=".venv/bin/python"

cmake --preset release >/dev/null
cmake --build --preset release --target lob_cli lob_bench >/dev/null

rm -rf _site
mkdir -p _site/data
cp web/index.html web/app.js web/styles.css _site/
cp docs/img/*.png _site/ 2>/dev/null || true

./build/release/tools/lob_cli demo --events 150000 --every 200 --depth 12 --out _site/data/replay.json
# The build machine's own benchmark run. Set SKIP_LIVE_BENCH=1 when building on a machine
# whose results are already committed under results/ (avoids a duplicate data set).
if [[ -z "${SKIP_LIVE_BENCH:-}" ]]; then
  ./build/release/bench/lob_bench $QUICK --json _site/data/bench-live.json >/dev/null
fi
cp results/bench-*.json _site/data/
"$PY" python/run_backtest.py --cli build/release/tools/lob_cli --seeds "${SEEDS:-30}" --horizon 3600 --out _site/data >/dev/null

# Optional: the real engine compiled to WebAssembly (needs Emscripten).
if command -v em++ >/dev/null 2>&1; then
  em++ -std=c++20 -O2 -Iinclude web/wasm/lob_wasm.cpp -o _site/lob_wasm.js \
    -sMODULARIZE=1 -sEXPORT_NAME=createLob -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=0 \
    -sEXPORTED_FUNCTIONS=_lob_reset,_lob_seed,_lob_submit,_lob_cancel,_lob_out_count,_lob_out_ptr,_lob_depth,_lob_best_bid,_lob_best_ask,_lob_order_count,_lob_open_qty \
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32
  echo "built WebAssembly engine"
else
  echo "em++ not found: skipping the WebAssembly build (the page hides that panel)"
fi

# Manifest tells the page which benchmark datasets exist.
( cd _site/data && printf '{"bench":[' && ls bench-*.json | sed 's/.*/"&"/' | paste -sd, - && printf ']}\n' ) > _site/data/manifest.json
echo "site ready in _site/"
