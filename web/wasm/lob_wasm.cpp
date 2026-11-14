// WebAssembly bindings: the same Book template, compiled with Emscripten, so
// the demo page runs the real matching engine in the visitor's browser.
// Plain C ABI; results are read from static buffers through the wasm heap.
#include <cstdint>

#include "lob/book.hpp"
#include "lob/flow_gen.hpp"

using namespace lob;

namespace {
constexpr BookConfig kCfg{2000, 4096};
constexpr int kMaxOut = 256, kMaxDepth = 16;
Book* g_book = nullptr;
FlowGenerator* g_gen = nullptr;
OutEvent g_out[kMaxOut];
int g_out_n = 0;
std::int32_t g_depth[2 * kMaxDepth * 2];  // [side][level] -> price, qty
OrderId g_next_id = 1ull << 32;           // user orders; seeded flow uses small ids

void collect(const OutEvent& o) {
    if (g_out_n < kMaxOut) g_out[g_out_n++] = o;
}
}  // namespace

extern "C" {

void lob_reset(std::uint32_t seed) {
    delete g_book;
    delete g_gen;
    g_book = new Book(kCfg);
    FlowConfig fc = FlowConfig::realistic(seed);
    fc.book = kCfg;
    fc.target_live = 400;
    fc.place_ppm = 150000;
    fc.max_dist = 40;
    fc.max_qty = 20;
    g_gen = new FlowGenerator(fc);
    g_out_n = 0;
}

// Feeds n synthetic background events; returns the number of trades they caused.
int lob_seed(int n) {
    int trades = 0;
    g_out_n = 0;
    for (int i = 0; i < n; ++i)
        g_book->apply(g_gen->next(), [&](const OutEvent& o) {
            trades += o.kind == OutKind::Trade;
            if (o.kind == OutKind::Trade) collect(o);
        });
    return trades;
}

// Submits a user order; returns its id. Output events are then readable via lob_out_*.
double lob_submit(int side, int type, int price, std::uint32_t qty, std::uint32_t owner) {
    InEvent e;
    e.id = g_next_id++;
    e.side = side ? Side::Sell : Side::Buy;
    e.type = static_cast<OrderType>(type < 0 || type > 4 ? 0 : type);
    e.price = price;
    e.qty = qty;
    e.owner = owner;
    g_out_n = 0;
    g_book->apply(e, collect);
    return static_cast<double>(e.id);
}

void lob_cancel(double id) {
    g_out_n = 0;
    g_book->cancel(static_cast<OrderId>(id), collect);
}

int lob_out_count() { return g_out_n; }
const OutEvent* lob_out_ptr() { return g_out; }  // 32-byte records, layout of lob::OutEvent

// Fills the depth buffer with up to `levels` levels per side; returns its address.
const std::int32_t* lob_depth(int levels) {
    if (levels > kMaxDepth) levels = kMaxDepth;
    for (auto& x : g_depth) x = 0;
    int side_index = 0;
    for (Side s : {Side::Buy, Side::Sell}) {
        int i = 0;
        g_book->for_each_level(s, levels, [&](Price p, std::uint64_t q) {
            g_depth[(side_index * kMaxDepth + i) * 2] = p;
            g_depth[(side_index * kMaxDepth + i) * 2 + 1] = static_cast<std::int32_t>(q);
            ++i;
        });
        ++side_index;
    }
    return g_depth;
}

int lob_best_bid() { return g_book->best_bid(); }
int lob_best_ask() { return g_book->best_ask(); }
int lob_order_count() { return static_cast<int>(g_book->order_count()); }
double lob_open_qty(double id) { return g_book->open_qty(static_cast<OrderId>(id)); }

}  // extern "C"
