// Small deterministic PRNG (xoshiro256**, seeded with splitmix64).
//
// The standard <random> distributions are implementation-defined, so the same
// seed gives different streams on libc++ and libstdc++. Everything here is
// integer arithmetic, so workloads are bit-identical on every platform.
#pragma once

#include <cstdint>

namespace lob {

class Rng {
public:
    explicit Rng(std::uint64_t seed) {
        for (auto& w : s_) {
            seed += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            w = z ^ (z >> 31);
        }
    }
    std::uint64_t next() noexcept {
        const std::uint64_t r = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }
    // Uniform in [0, n) for n < 2^32 (multiply-shift; bias < 2^-32, irrelevant here).
    std::uint32_t below(std::uint32_t n) noexcept {
        return static_cast<std::uint32_t>(((next() >> 32) * n) >> 32);
    }
    // True with probability ppm / 1e6.
    bool chance_ppm(std::uint32_t ppm) noexcept { return below(1000000) < ppm; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4];
};

}  // namespace lob
