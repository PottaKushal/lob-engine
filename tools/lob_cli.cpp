// Command-line front end.
//
//   lob_cli gen    --events N [--seed S] --out FILE     write a synthetic event log
//   lob_cli run    FILE [--trades T.csv] [--snapshots S.jsonl --every K --depth D]
//   lob_cli verify FILE                                 replay on every book, compare hashes
//   lob_cli stream                                      binary events on stdin, binary output on stdout
//   lob_cli demo   --out DIR [--events N] [--seed S]    JSON for the web visualizer
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lob/book.hpp"
#include "lob/book_list.hpp"
#include "lob/flow_gen.hpp"
#include "lob/hash.hpp"
#include "lob/protocol.hpp"

using namespace lob;

namespace {

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "lob_cli: %s\n", msg.c_str());
    std::exit(1);
}

struct Args {
    std::vector<std::string> pos;
    std::size_t events = 1000000, every = 0;
    std::uint64_t seed = 1;
    int depth = 10;
    std::string out, trades, snapshots;
};
Args parse(int argc, char** argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value for " + s);
            return argv[++i];
        };
        if (s == "--events") a.events = std::strtoull(val().c_str(), nullptr, 10);
        else if (s == "--seed") a.seed = std::strtoull(val().c_str(), nullptr, 10);
        else if (s == "--every") a.every = std::strtoull(val().c_str(), nullptr, 10);
        else if (s == "--depth") a.depth = std::atoi(val().c_str());
        else if (s == "--out") a.out = val();
        else if (s == "--trades") a.trades = val();
        else if (s == "--snapshots") a.snapshots = val();
        else if (s.rfind("--", 0) == 0) die("unknown option " + s);
        else a.pos.push_back(s);
    }
    return a;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) die("cannot open " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Log {
    BookConfig cfg;
    std::vector<InEvent> events;
};
Log load(const std::string& path) {
    const std::vector<std::uint8_t> bytes = read_file(path);
    Log log;
    const proto::LogResult r = proto::decode_log(bytes.data(), bytes.size(), log.cfg, log.events);
    if (r.status != proto::Status::Ok)
        die("malformed log at byte " + std::to_string(r.offset) + " (status " + std::to_string(static_cast<int>(r.status)) + ")");
    return log;
}

template <class F>
void write_levels(std::FILE* f, const Book& book, Side s, int depth, F&& sep) {
    bool first = true;
    book.for_each_level(s, depth, [&](Price p, std::uint64_t q) {
        std::fprintf(f, "%s[%d,%llu]", first ? "" : sep, p, static_cast<unsigned long long>(q));
        first = false;
    });
}

int cmd_gen(const Args& a) {
    if (a.out.empty()) die("gen needs --out FILE");
    const FlowConfig cfg = FlowConfig::realistic(a.seed);
    const std::vector<InEvent> ev = FlowGenerator(cfg).generate(a.events);
    const std::vector<std::uint8_t> bytes = proto::encode_log(cfg.book, ev);
    std::ofstream f(a.out, std::ios::binary);
    if (!f) die("cannot write " + a.out);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::printf("wrote %zu events, %zu bytes to %s\n", ev.size(), bytes.size(), a.out.c_str());
    return 0;
}

int cmd_run(const Args& a) {
    if (a.pos.empty()) die("run needs a log file");
    const Log log = load(a.pos[0]);
    Book book(log.cfg);
    std::FILE* tf = a.trades.empty() ? nullptr : std::fopen(a.trades.c_str(), "w");
    std::FILE* sf = a.snapshots.empty() ? nullptr : std::fopen(a.snapshots.c_str(), "w");
    if (!a.trades.empty() && !tf) die("cannot write " + a.trades);
    if (!a.snapshots.empty() && !sf) die("cannot write " + a.snapshots);
    if (tf) std::fprintf(tf, "ts,taker_id,maker_id,taker_side,price,qty\n");
    Fnv out_hash;
    std::size_t trades = 0, outs = 0, n = 0;
    for (const InEvent& e : log.events) {
        book.apply(e, [&](const OutEvent& o) {
            out_hash.mix(o);
            ++outs;
            if (o.kind != OutKind::Trade) return;
            ++trades;
            if (tf)
                std::fprintf(tf, "%llu,%llu,%llu,%c,%d,%u\n", static_cast<unsigned long long>(e.ts),
                             static_cast<unsigned long long>(o.id), static_cast<unsigned long long>(o.other),
                             o.side == Side::Buy ? 'B' : 'S', o.price, o.qty);
        });
        ++n;
        if (sf && a.every && n % a.every == 0) {
            std::fprintf(sf, "{\"seq\":%zu,\"ts\":%llu,\"bids\":[", n, static_cast<unsigned long long>(e.ts));
            write_levels(sf, book, Side::Buy, a.depth, ",");
            std::fprintf(sf, "],\"asks\":[");
            write_levels(sf, book, Side::Sell, a.depth, ",");
            std::fprintf(sf, "]}\n");
        }
    }
    if (tf) std::fclose(tf);
    if (sf) std::fclose(sf);
    std::printf("events=%zu out_events=%zu trades=%zu resting=%zu best_bid=%d best_ask=%d\n", n, outs, trades,
                book.order_count(), book.best_bid(), book.best_ask());
    std::printf("output_hash=%016llx state_hash=%016llx\n", static_cast<unsigned long long>(out_hash.h),
                static_cast<unsigned long long>(state_hash(book)));
    return 0;
}

template <class BookT>
std::pair<std::uint64_t, std::uint64_t> replay_hashes(const Log& log) {
    BookT book(log.cfg);
    Fnv f;
    for (const InEvent& e : log.events) book.apply(e, [&](const OutEvent& o) { f.mix(o); });
    return {f.h, state_hash(book)};
}

int cmd_verify(const Args& a) {
    if (a.pos.empty()) die("verify needs a log file");
    const Log log = load(a.pos[0]);
    const auto h1 = replay_hashes<BookV1>(log), h2 = replay_hashes<BookV2>(log), h3 = replay_hashes<BookV3>(log),
               h4 = replay_hashes<BookV3Bitmap>(log), again = replay_hashes<BookV3Bitmap>(log);
    std::printf("events=%zu output_hash=%016llx state_hash=%016llx\n", log.events.size(),
                static_cast<unsigned long long>(h4.first), static_cast<unsigned long long>(h4.second));
    const bool ok = h1 == h2 && h2 == h3 && h3 == h4 && h4 == again;
    std::printf("%s\n", ok ? "OK: v1, v2, v3 and v3+bitmap agree, and a second replay is identical"
                           : "MISMATCH between book versions");
    return ok ? 0 : 1;
}

// Streaming mode for the Python backtester. Reads a log header then messages
// from stdin. Output events are buffered; on each sync marker ('S') it writes
// them, followed by one top-of-book record, and flushes.
int cmd_stream() {
    std::vector<std::uint8_t> buf;
    std::size_t off = 0;
    auto fill = [&]() -> bool {  // read more input; false on EOF
        if (off > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(off));
            off = 0;
        }
        std::uint8_t tmp[1 << 16];
        // read(2), not fread: fread would block until the whole buffer is full,
        // and the client is waiting for our reply to a much shorter batch.
        ssize_t n;
        do n = ::read(STDIN_FILENO, tmp, sizeof tmp);
        while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + n);
        return true;
    };
    BookConfig cfg;
    while (buf.size() < proto::kHeaderSize)
        if (!fill()) die("stream: missing header");
    if (proto::decode_header(buf.data(), buf.size(), cfg) != proto::Status::Ok) die("stream: bad header");
    off = proto::kHeaderSize;
    Book book(cfg);
    std::vector<std::uint8_t> out;
    auto emit = [&](const OutEvent& o) {
        out.resize(out.size() + proto::kOutSize);
        proto::encode_out(o, out.data() + out.size() - proto::kOutSize);
    };
    for (;;) {
        InEvent e;
        const proto::Decoded d = proto::decode(buf.data() + off, buf.size() - off, e);
        if (d.status == proto::Status::Truncated) {
            if (!fill()) break;
            continue;
        }
        if (d.status == proto::Status::Ok) {
            book.apply(e, emit);
        } else if (d.status == proto::Status::Sync) {
            OutEvent top;
            top.kind = static_cast<OutKind>(proto::kOutBookTop);
            top.price = book.best_bid();
            top.qty = static_cast<Qty>(book.best_ask());
            top.id = book.level_qty(Side::Buy, book.best_bid());
            top.other = book.level_qty(Side::Sell, book.best_ask());
            emit(top);
            std::fwrite(out.data(), 1, out.size(), stdout);
            std::fflush(stdout);
            out.clear();
        } else {
            die("stream: malformed message (status " + std::to_string(static_cast<int>(d.status)) + ")");
        }
        off += d.consumed;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) die("usage: lob_cli gen|run|verify|stream|demo [options]");
    const std::string cmd = argv[1];
    const Args a = parse(argc, argv);
    if (cmd == "gen") return cmd_gen(a);
    if (cmd == "run") return cmd_run(a);
    if (cmd == "verify") return cmd_verify(a);
    if (cmd == "stream") return cmd_stream();
    die("unknown command " + cmd);
}
