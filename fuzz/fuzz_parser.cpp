// Fuzz target: arbitrary bytes -> decoder -> book.
//
// Checks, for any input:
//   * the decoder never reads out of bounds (ASan) and has no UB (UBSan);
//   * every decoded message survives an encode/decode round trip unchanged;
//   * the fast book and the brute-force reference agree on every output
//     event and on the final resting state (differential fuzzing).
//
// Built with libFuzzer when LOB_BUILD_FUZZ=ON (LLVM clang). The same entry
// point is also driven by a deterministic mutation loop in the unit tests,
// so it runs on every platform, including Apple clang which has no libFuzzer.
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "lob/book.hpp"
#include "lob/hash.hpp"
#include "lob/protocol.hpp"
#include "lob/reference_book.hpp"

using namespace lob;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Header path: must reject or accept without crashing.
    BookConfig hdr;
    std::vector<InEvent> evs;
    (void)proto::decode_log(data, size, hdr, evs);

    // Message path, on a deliberately tiny book so the fuzzer reaches full-book and empty-book states.
    const BookConfig cfg{64, 32};
    Book fast(cfg);
    ReferenceBook ref(cfg);
    std::vector<OutEvent> a, b;
    std::size_t off = 0;
    while (off < size) {
        InEvent e;
        const proto::Decoded d = proto::decode(data + off, size - off, e);
        if (d.status == proto::Status::Sync) { off += d.consumed; continue; }
        if (d.status != proto::Status::Ok) break;
        if (d.consumed == 0 || d.consumed > size - off) std::abort();

        std::uint8_t tmp[proto::kMaxMsgSize];
        InEvent e2;
        const std::size_t n = proto::encode(e, tmp);
        if (n != d.consumed || proto::decode(tmp, n, e2).status != proto::Status::Ok || !(e == e2)) std::abort();

        a.clear();
        b.clear();
        fast.apply(e, [&](const OutEvent& o) { a.push_back(o); });
        ref.apply(e, [&](const OutEvent& o) { b.push_back(o); });
        if (a != b || fast.best_bid() != ref.best_bid() || fast.best_ask() != ref.best_ask()) std::abort();
        off += d.consumed;
    }
    if (state_hash(fast) != state_hash(ref)) std::abort();
    return 0;
}
