// Benchmark harness. Prints markdown tables and optionally writes JSON.
//
//   lob_bench [--events N] [--runs R] [--seed S] [--json FILE] [--pin] [--quick]
//   lob_bench --profile v1|v2|v3|v3b [--seconds S]   replay one book in a loop for a profiler
//
// Methodology (details in BENCHMARKS.md):
//   * The event stream is generated once, up front, from a fixed seed.
//   * Every book replays the same stream from memory on one thread.
//   * Throughput: one untimed warmup replay, then R timed replays on a fresh
//     book each; total wall time over all events. No per-event clock calls.
//   * Latency: separate replays with a clock read before and after each
//     event, recorded in a log-linear histogram; the first 10% is skipped.
//   * Pipeline: wire bytes in, encoded events out, serial versus three
//     threads; latency measured open-loop at fixed arrival rates.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_list.hpp"
#include "lob/flow_gen.hpp"
#include "lob/histogram.hpp"
#include "lob/pipeline.hpp"
#include "lob/protocol.hpp"

using namespace lob;

namespace {

struct Opts {
    std::size_t events = 5000000;
    int runs = 7;
    std::uint64_t seed = 1;
    std::string json;
    bool pin = false;
    std::string profile;  // replay one book in a loop so a sampling profiler can attach
    int seconds = 10;
};

std::string sh(const char* cmd) {
    std::string out;
    if (FILE* f = popen(cmd, "r")) {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) out += buf;
        pclose(f);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    const std::size_t b = out.find_first_not_of(" \t");
    return b == std::string::npos ? "" : out.substr(b);
}

std::string compiler() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

struct Spread {
    double median = 0, min = 0, max = 0, cv_pct = 0;
};
Spread spread(std::vector<double> v) {
    Spread s;
    std::sort(v.begin(), v.end());
    s.min = v.front();
    s.max = v.back();
    s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    double mean = 0, var = 0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    for (double x : v) var += (x - mean) * (x - mean);
    s.cv_pct = v.size() > 1 ? 100.0 * std::sqrt(var / static_cast<double>(v.size() - 1)) / mean : 0;
    return s;
}

struct Lat {
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    double mean = 0;
};
Lat lat_of(const Histogram& h) {
    return {h.percentile(0.50), h.percentile(0.90), h.percentile(0.99), h.percentile(0.999), h.percentile(0.9999),
            h.max(), h.mean()};
}
std::string lat_json(const Lat& l) {
    std::ostringstream o;
    o << "{\"p50\":" << l.p50 << ",\"p90\":" << l.p90 << ",\"p99\":" << l.p99 << ",\"p999\":" << l.p999
      << ",\"p9999\":" << l.p9999 << ",\"max\":" << l.max << ",\"mean\":" << l.mean << "}";
    return o.str();
}
std::string spread_json(const Spread& s) {
    std::ostringstream o;
    o << "{\"median\":" << s.median << ",\"min\":" << s.min << ",\"max\":" << s.max << ",\"cv_pct\":" << s.cv_pct << "}";
    return o.str();
}

struct BookResult {
    std::string name, desc;
    Spread mps;  // million events per second
    Lat lat;
    std::uint64_t checksum = 0;
};

template <class BookT>
BookResult bench_book(const char* name, const char* desc, const std::vector<InEvent>& ev, const BookConfig& cfg,
                      int runs) {
    BookResult r;
    r.name = name;
    r.desc = desc;
    std::vector<double> mps;
    for (int run = -1; run < runs; ++run) {  // run -1 is the warmup
        BookT book(cfg);
        std::uint64_t chk = 0;
        auto sink = [&](const OutEvent& o) { chk += o.id + o.qty + static_cast<std::uint64_t>(o.kind); };
        const std::uint64_t t0 = now_ns();
        for (const InEvent& e : ev) book.apply(e, sink);
        const std::uint64_t t1 = now_ns();
        r.checksum = chk;
        if (run >= 0) mps.push_back(static_cast<double>(ev.size()) / (static_cast<double>(t1 - t0) * 1e-9) / 1e6);
    }
    r.mps = spread(mps);

    Histogram h;
    const std::size_t skip = ev.size() / 10;
    for (int run = 0; run < 3; ++run) {
        BookT book(cfg);
        std::uint64_t chk = 0;
        auto sink = [&](const OutEvent& o) { chk += o.qty; };
        for (std::size_t i = 0; i < ev.size(); ++i) {
            const std::uint64_t a = now_ns();
            book.apply(ev[i], sink);
            const std::uint64_t b = now_ns();
            if (i >= skip) h.record(b - a);
        }
        r.checksum += chk & 0;
    }
    r.lat = lat_of(h);
    return r;
}

void print_books(const char* title, const std::vector<BookResult>& rs) {
    std::printf("\n### %s\n\n", title);
    std::printf("| Version | Throughput, M events/s (median) | min..max | CV | p50 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |\n");
    std::printf("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (const BookResult& r : rs)
        std::printf("| %s | %.2f | %.2f..%.2f | %.1f%% | %llu | %llu | %llu | %llu | %llu |\n", r.name.c_str(), r.mps.median,
                    r.mps.min, r.mps.max, r.mps.cv_pct, static_cast<unsigned long long>(r.lat.p50),
                    static_cast<unsigned long long>(r.lat.p99), static_cast<unsigned long long>(r.lat.p999),
                    static_cast<unsigned long long>(r.lat.p9999), static_cast<unsigned long long>(r.lat.max));
}

std::string books_json(const std::vector<BookResult>& rs) {
    std::ostringstream o;
    o << "[";
    for (std::size_t i = 0; i < rs.size(); ++i)
        o << (i ? "," : "") << "{\"name\":\"" << rs[i].name << "\",\"desc\":\"" << rs[i].desc
          << "\",\"mps\":" << spread_json(rs[i].mps) << ",\"latency_ns\":" << lat_json(rs[i].lat) << "}";
    o << "]";
    return o.str();
}

std::vector<BookResult> bench_all(const std::vector<InEvent>& ev, const BookConfig& cfg, int runs) {
    std::vector<BookResult> rs;
    rs.push_back(bench_book<BookV1>("v1 map+list", "std::map levels, std::list orders, unordered_map index", ev, cfg, runs));
    rs.push_back(bench_book<BookV2>("v2 flat levels", "flat price array, std::list orders, unordered_map index", ev, cfg, runs));
    rs.push_back(bench_book<BookV3>("v3 pool", "flat price array, pooled intrusive orders, open-addressing index", ev, cfg, runs));
    rs.push_back(bench_book<BookV3Bitmap>("v3 pool+bitmap", "v3 plus a bitmap of non-empty levels", ev, cfg, runs));
    for (const BookResult& r : rs)
        if (r.checksum != rs[0].checksum) {
            std::fprintf(stderr, "checksum mismatch between book versions\n");
            std::exit(1);
        }
    return rs;
}

struct Profile {
    std::size_t n_new = 0, n_cancel = 0, n_modify = 0, trades = 0, outs = 0, resting = 0, levels = 0;
};
Profile profile(const std::vector<InEvent>& ev, const BookConfig& cfg) {
    Profile p;
    Book book(cfg);
    for (const InEvent& e : ev) {
        p.n_new += e.kind == InKind::New;
        p.n_cancel += e.kind == InKind::Cancel;
        p.n_modify += e.kind == InKind::Modify;
        book.apply(e, [&](const OutEvent& o) {
            ++p.outs;
            p.trades += o.kind == OutKind::Trade;
        });
    }
    p.resting = book.order_count();
    for (Side s : {Side::Buy, Side::Sell}) book.for_each_level(s, 1 << 30, [&](Price, std::uint64_t) { ++p.levels; });
    return p;
}
std::string profile_json(const Profile& p, std::size_t n) {
    std::ostringstream o;
    o << "{\"events\":" << n << ",\"new\":" << p.n_new << ",\"cancel\":" << p.n_cancel << ",\"modify\":" << p.n_modify
      << ",\"trades\":" << p.trades << ",\"out_events\":" << p.outs << ",\"resting_orders_end\":" << p.resting
      << ",\"levels_end\":" << p.levels << "}";
    return o.str();
}
void print_profile(const char* name, const Profile& p, std::size_t n) {
    std::printf("- %s: %zu events (%.1f%% new, %.1f%% cancel, %.1f%% modify), %zu trades, %zu resting orders on %zu levels at end\n",
                name, n, 100.0 * static_cast<double>(p.n_new) / static_cast<double>(n),
                100.0 * static_cast<double>(p.n_cancel) / static_cast<double>(n),
                100.0 * static_cast<double>(p.n_modify) / static_cast<double>(n), p.trades, p.resting, p.levels);
}

template <class BookT>
void profile_loop(const std::vector<InEvent>& ev, const BookConfig& cfg, int seconds) {
    const std::uint64_t end = now_ns() + static_cast<std::uint64_t>(seconds) * 1000000000ull;
    std::uint64_t chk = 0, replays = 0;
    while (now_ns() < end) {
        BookT book(cfg);
        for (const InEvent& e : ev) book.apply(e, [&](const OutEvent& o) { chk += o.qty; });
        ++replays;
    }
    std::printf("profile loop: %llu replays, checksum %llu\n", static_cast<unsigned long long>(replays),
                static_cast<unsigned long long>(chk));
}

}  // namespace

int main(int argc, char** argv) {
    Opts opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--events") opt.events = std::strtoull(val(), nullptr, 10);
        else if (a == "--runs") opt.runs = std::atoi(val());
        else if (a == "--seed") opt.seed = std::strtoull(val(), nullptr, 10);
        else if (a == "--json") opt.json = val();
        else if (a == "--pin") opt.pin = true;
        else if (a == "--profile") opt.profile = val();
        else if (a == "--seconds") opt.seconds = std::atoi(val());
        else if (a == "--quick") { opt.events = 500000; opt.runs = 3; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (opt.events < 1000 || opt.runs < 1) { std::fprintf(stderr, "need --events >= 1000 and --runs >= 1\n"); return 2; }
    prefer_fast_core(opt.pin ? 1 : -1);

    if (!opt.profile.empty()) {
        const FlowConfig fc = FlowConfig::realistic(opt.seed);
        const std::vector<InEvent> ev = FlowGenerator(fc).generate(opt.events);
        if (opt.profile == "v1") profile_loop<BookV1>(ev, fc.book, opt.seconds);
        else if (opt.profile == "v2") profile_loop<BookV2>(ev, fc.book, opt.seconds);
        else if (opt.profile == "v3") profile_loop<BookV3>(ev, fc.book, opt.seconds);
        else profile_loop<BookV3Bitmap>(ev, fc.book, opt.seconds);
        return 0;
    }

    // --- environment --------------------------------------------------------
#if defined(__APPLE__)
    const std::string cpu = sh("sysctl -n machdep.cpu.brand_string");
#else
    const std::string cpu = sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2");
#endif
    const std::string os = sh("uname -srm");
    const unsigned cores = std::thread::hardware_concurrency();
    std::uint64_t res = ~0ull, last = now_ns();
    const std::uint64_t c0 = now_ns();
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t t = now_ns();
        if (t != last) res = std::min(res, t - last);
        last = t;
    }
    const double clock_cost = static_cast<double>(now_ns() - c0) / 200000.0;
    char date[32];
    const std::time_t tt = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y-%m-%d", std::gmtime(&tt));

    std::printf("## lob_bench\n\n");
    std::printf("- CPU: %s (%u logical cores)\n- OS: %s\n- Compiler: %s\n- Build: %s, flags: `%s`\n", cpu.c_str(), cores,
                os.c_str(), compiler().c_str(), LOB_BUILD_TYPE, LOB_BUILD_FLAGS);
    std::printf("- Clock: steady_clock, resolution %llu ns, about %.0f ns per call\n", static_cast<unsigned long long>(res), clock_cost);
    std::printf("- Events per replay: %zu, timed runs: %d, seed: %llu, pinned: %s\n", opt.events, opt.runs,
                static_cast<unsigned long long>(opt.seed), opt.pin ? "yes" : "no");

    // --- workloads ----------------------------------------------------------
    const FlowConfig dense_cfg = FlowConfig::realistic(opt.seed);
    FlowConfig sparse_cfg = FlowConfig::realistic(opt.seed);
    sparse_cfg.place_ppm = 1500;   // orders land hundreds of ticks apart
    sparse_cfg.max_dist = 6000;
    sparse_cfg.target_live = 1500;
    const std::vector<InEvent> dense = FlowGenerator(dense_cfg).generate(opt.events);
    const std::vector<InEvent> sparse = FlowGenerator(sparse_cfg).generate(opt.events);
    const Profile pd = profile(dense, dense_cfg.book), ps = profile(sparse, sparse_cfg.book);
    std::printf("\nWorkloads (synthetic, seeded):\n");
    print_profile("dense", pd, dense.size());
    print_profile("sparse", ps, sparse.size());

    // --- single-threaded books ---------------------------------------------
    const std::vector<BookResult> rd = bench_all(dense, dense_cfg.book, opt.runs);
    print_books("Matching core, dense book (single thread, events already decoded)", rd);
    const std::vector<BookResult> rs = bench_all(sparse, sparse_cfg.book, opt.runs);
    print_books("Matching core, sparse book", rs);

    // Worst case for a flat price array: the best bid empties while the next bid is
    // 59,900 ticks away, over and over. One far order rests; a near order is added and cancelled.
    std::vector<InEvent> gap;
    {
        InEvent far;
        far.id = 1; far.side = Side::Buy; far.price = 100; far.qty = 1;
        gap.push_back(far);
        for (std::size_t i = 0; gap.size() + 2 <= std::max<std::size_t>(opt.events / 20, 1000); ++i) {
            InEvent a;
            a.id = 2 + i; a.side = Side::Buy; a.price = 60000; a.qty = 1;
            InEvent c;
            c.kind = InKind::Cancel; c.id = a.id;
            gap.push_back(a);
            gap.push_back(c);
        }
    }
    const std::vector<BookResult> rg = bench_all(gap, dense_cfg.book, opt.runs);
    print_books("Matching core, worst-case gap (best level empties, next level 59,900 ticks away)", rg);

    // --- wire to wire: serial versus pipeline --------------------------------
    std::vector<std::uint8_t> log = proto::encode_log(dense_cfg.book, dense);
    const std::uint8_t* wire = log.data() + proto::kHeaderSize;
    const std::size_t wire_len = log.size() - proto::kHeaderSize;
    std::uint64_t pub_chk = 0;
    std::uint8_t scratch[proto::kOutSize];
    auto publish = [&](const OutEvent& o) {  // serialize every output event
        proto::encode_out(o, scratch);
        pub_chk += proto::get(scratch, 8) ^ proto::get(scratch + 16, 8);
    };
    std::vector<double> serial_mps, pipe_mps;
    std::uint64_t serial_chk = 0;
    for (int run = -1; run < opt.runs; ++run) {
        Book book(dense_cfg.book);
        pub_chk = 0;
        const PipelineStats st = run_serial(book, wire, wire_len, publish);
        serial_chk = pub_chk;
        if (run >= 0) serial_mps.push_back(static_cast<double>(st.events_in) / st.seconds / 1e6);
    }
    PipelineOptions po;
    po.pin = opt.pin;
    po.spin_limit = ~0u;  // dedicated cores: busy spin, never yield
    for (int run = -1; run < opt.runs; ++run) {
        Book book(dense_cfg.book);
        pub_chk = 0;
        const PipelineStats st = run_pipeline(book, wire, wire_len, publish, po);
        if (pub_chk != serial_chk || st.events_in != dense.size()) {
            std::fprintf(stderr, "pipeline output differs from serial output\n");
            return 1;
        }
        if (run >= 0) pipe_mps.push_back(static_cast<double>(st.events_in) / st.seconds / 1e6);
    }
    const Spread ss = spread(serial_mps), sp = spread(pipe_mps);
    std::printf("\n### Wire to wire, dense book (decode, match, encode every output)\n\n");
    std::printf("| Configuration | Throughput, M msgs/s (median) | min..max | CV |\n|---|---:|---:|---:|\n");
    std::printf("| serial, 1 thread | %.2f | %.2f..%.2f | %.1f%% |\n", ss.median, ss.min, ss.max, ss.cv_pct);
    std::printf("| v4 pipeline, 3 threads + 2 SPSC queues | %.2f | %.2f..%.2f | %.1f%% |\n", sp.median, sp.min, sp.max, sp.cv_pct);

    // --- pipeline latency at fixed arrival rates -----------------------------
    std::printf("\n### v4 pipeline latency, open loop (scheduled send to publish)\n\n");
    std::printf("| Arrival rate, msgs/s | Messages | p50 ns | p90 ns | p99 ns | p99.9 ns | p99.99 ns | max ns |\n|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    std::ostringstream paced_json;
    paced_json << "[";
    bool first = true;
    for (const std::uint64_t rate : {100000ull, 500000ull, 1000000ull, 2000000ull, 4000000ull}) {
        if (static_cast<double>(rate) > 0.6 * sp.median * 1e6) continue;  // stay well below saturation
        const std::size_t n = std::min<std::size_t>(dense.size(), rate * 4);  // about 4 seconds
        std::size_t bytes = 0;
        {  // length of the first n messages
            InEvent tmp;
            for (std::size_t i = 0; i < n; ++i) bytes += proto::decode(wire + bytes, wire_len - bytes, tmp).consumed;
        }
        PipelineOptions pl = po;
        pl.period_ns = 1000000000ull / rate;
        pl.measure_latency = true;
        pl.latency_skip = n / 10;
        Histogram h;
        for (int run = 0; run < 3; ++run) {
            Book book(dense_cfg.book);
            h.merge(run_pipeline(book, wire, bytes, publish, pl).latency);
        }
        const Lat l = lat_of(h);
        std::printf("| %llu | %llu | %llu | %llu | %llu | %llu | %llu | %llu |\n", static_cast<unsigned long long>(rate),
                    static_cast<unsigned long long>(h.count()), static_cast<unsigned long long>(l.p50),
                    static_cast<unsigned long long>(l.p90), static_cast<unsigned long long>(l.p99),
                    static_cast<unsigned long long>(l.p999), static_cast<unsigned long long>(l.p9999),
                    static_cast<unsigned long long>(l.max));
        paced_json << (first ? "" : ",") << "{\"rate\":" << rate << ",\"samples\":" << h.count() << ",\"latency_ns\":" << lat_json(l) << "}";
        first = false;
    }
    paced_json << "]";

    if (!opt.json.empty()) {
        std::ofstream f(opt.json);
        f << "{\n \"env\":{\"cpu\":\"" << cpu << "\",\"cores\":" << cores << ",\"os\":\"" << os << "\",\"compiler\":\""
          << compiler() << "\",\"build\":\"" << LOB_BUILD_TYPE << "\",\"flags\":\"" << LOB_BUILD_FLAGS
          << "\",\"clock_resolution_ns\":" << res << ",\"clock_cost_ns\":" << clock_cost << ",\"date\":\"" << date
          << "\",\"runs\":" << opt.runs << ",\"seed\":" << opt.seed << ",\"pinned\":" << (opt.pin ? "true" : "false") << "},\n"
          << " \"workloads\":{\"dense\":" << profile_json(pd, dense.size()) << ",\"sparse\":" << profile_json(ps, sparse.size()) << "},\n"
          << " \"books_dense\":" << books_json(rd) << ",\n \"books_sparse\":" << books_json(rs) << ",\n \"books_gap\":" << books_json(rg) << ",\n"
          << " \"wire\":{\"serial_mps\":" << spread_json(ss) << ",\"pipeline_mps\":" << spread_json(sp) << "},\n"
          << " \"pipeline_latency\":" << paced_json.str() << "\n}\n";
        std::printf("\nwrote %s\n", opt.json.c_str());
    }
    return 0;
}
